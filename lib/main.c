/*
 * QuickJS stand alone interpreter
 *
 * Copyright (c) 2017-2021 Fabrice Bellard
 * Copyright (c) 2017-2021 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif

#include "../quickjs/cutils.h"
#include "../quickjs/quickjs-libc.h"
#include "./source.h"

extern const uint8_t qjsc_repl[];
extern const uint32_t qjsc_repl_size;
#ifdef CONFIG_BIGNUM
extern const uint8_t qjsc_qjscalc[];
extern const uint32_t qjsc_qjscalc_size;
static int bignum_ext;
#endif

static int eval_buf(JSContext *ctx, const void *buf, int buf_len,
                    const char *filename, int eval_flags) {
  JSValue val;
  int ret;

  if ((eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE) {
    /* for the modules, we compile then run to be able to set
       import.meta */
    val = JS_Eval(ctx, buf, buf_len, filename,
                  eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
    if (!JS_IsException(val)) {
      js_module_set_import_meta(ctx, val, TRUE, TRUE);
      val = JS_EvalFunction(ctx, val);
    }
  } else {
    val = JS_Eval(ctx, buf, buf_len, filename, eval_flags);
  }
  if (JS_IsException(val)) {
    js_std_dump_error(ctx);
    ret = -1;
  } else {
    ret = 0;
  }
  JS_FreeValue(ctx, val);
  return ret;
}

/* also used to initialize the worker context */
static JSContext *JS_NewCustomContext(JSRuntime *rt) {
  JSContext *ctx;
  ctx = JS_NewContext(rt);
  if (!ctx) return NULL;
#ifdef CONFIG_BIGNUM
  if (bignum_ext) {
    JS_AddIntrinsicBigFloat(ctx);
    JS_AddIntrinsicBigDecimal(ctx);
    JS_AddIntrinsicOperators(ctx);
    JS_EnableBignumExt(ctx, TRUE);
  }
#endif
  /* system modules */
  js_init_module_std(ctx, "std");
  js_init_module_os(ctx, "os");
  return ctx;
}

#if defined(__APPLE__)
#define MALLOC_OVERHEAD 0
#else
#define MALLOC_OVERHEAD 8
#endif

struct trace_malloc_data {
  uint8_t *base;
};

static inline unsigned long long js_trace_malloc_ptr_offset(
    uint8_t *ptr, struct trace_malloc_data *dp) {
  return ptr - dp->base;
}

/* default memory allocation functions with memory limitation */
static inline size_t js_trace_malloc_usable_size(void *ptr) {
#if defined(__APPLE__)
  return malloc_size(ptr);
#elif defined(_WIN32)
  return _msize(ptr);
#elif defined(EMSCRIPTEN)
  return 0;
#elif defined(__wasi__)
  return 0;
#elif defined(__linux__)
  return malloc_usable_size(ptr);
#else
  /* change this to `return 0;` if compilation fails */
  return malloc_usable_size(ptr);
#endif
}

static void
#ifdef _WIN32
    /* mingw printf is used */
    __attribute__((format(gnu_printf, 2, 3)))
#else
    __attribute__((format(printf, 2, 3)))
#endif
    js_trace_malloc_printf(JSMallocState *s, const char *fmt, ...) {
  va_list ap;
  int c;

  va_start(ap, fmt);
  while ((c = *fmt++) != '\0') {
    if (c == '%') {
      /* only handle %p and %zd */
      if (*fmt == 'p') {
        uint8_t *ptr = va_arg(ap, void *);
        if (ptr == NULL) {
          printf("NULL");
        } else {
          printf("H%+06lld.%zd", js_trace_malloc_ptr_offset(ptr, s->opaque),
                 js_trace_malloc_usable_size(ptr));
        }
        fmt++;
        continue;
      }
      if (fmt[0] == 'z' && fmt[1] == 'd') {
        size_t sz = va_arg(ap, size_t);
        printf("%zd", sz);
        fmt += 2;
        continue;
      }
    }
    putc(c, stdout);
  }
  va_end(ap);
}

static void js_trace_malloc_init(struct trace_malloc_data *s) {
  free(s->base = malloc(8));
}

static void *js_trace_malloc(JSMallocState *s, size_t size) {
  void *ptr;

  /* Do not allocate zero bytes: behavior is platform dependent */
  assert(size != 0);

  if (unlikely(s->malloc_size + size > s->malloc_limit)) return NULL;
  ptr = malloc(size);
  js_trace_malloc_printf(s, "A %zd -> %p\n", size, ptr);
  if (ptr) {
    s->malloc_count++;
    s->malloc_size += js_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
  }
  return ptr;
}

static void js_trace_free(JSMallocState *s, void *ptr) {
  if (!ptr) return;

  js_trace_malloc_printf(s, "F %p\n", ptr);
  s->malloc_count--;
  s->malloc_size -= js_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
  free(ptr);
}

static void *js_trace_realloc(JSMallocState *s, void *ptr, size_t size) {
  size_t old_size;

  if (!ptr) {
    if (size == 0) return NULL;
    return js_trace_malloc(s, size);
  }
  old_size = js_trace_malloc_usable_size(ptr);
  if (size == 0) {
    js_trace_malloc_printf(s, "R %zd %p\n", size, ptr);
    s->malloc_count--;
    s->malloc_size -= old_size + MALLOC_OVERHEAD;
    free(ptr);
    return NULL;
  }
  if (s->malloc_size + size - old_size > s->malloc_limit) return NULL;

  js_trace_malloc_printf(s, "R %zd %p", size, ptr);

  ptr = realloc(ptr, size);
  js_trace_malloc_printf(s, " -> %p\n", ptr);
  if (ptr) {
    s->malloc_size += js_trace_malloc_usable_size(ptr) - old_size;
  }
  return ptr;
}

static const JSMallocFunctions trace_mf = {
    js_trace_malloc,
    js_trace_free,
    js_trace_realloc,
#if defined(__APPLE__)
    malloc_size,
#elif defined(_WIN32)
    (size_t(*)(const void *))_msize,
#elif defined(EMSCRIPTEN)
    NULL,
#elif defined(__wasi__)
    NULL,
#elif defined(__linux__)
    (size_t(*)(const void *))malloc_usable_size,
#else
    /* change this to `NULL,` if compilation fails */
    malloc_usable_size,
#endif
};

int main() {
  JSRuntime *rt;
  JSContext *ctx;

  struct trace_malloc_data trace_data = {NULL};
  int optind;
  char *expr = NULL;
  int interactive = 0;
  int dump_memory = 0;
  int trace_memory = 0;
  int empty_run = 0;
  int module = 1;
  int load_std = 0;
  int dump_unhandled_promise_rejection = 0;
  size_t memory_limit = 0;
  char *include_list[32];
  int i, include_count = 0;
#ifdef CONFIG_BIGNUM
  int load_jscalc;
#endif
  size_t stack_size = 0;

  if (trace_memory) {
    js_trace_malloc_init(&trace_data);
    rt = JS_NewRuntime2(&trace_mf, &trace_data);
  } else {
    rt = JS_NewRuntime();
  }

  if (!rt) {
    fprintf(stderr, "qjs: cannot allocate JS runtime\n");
    exit(2);
  }
  if (memory_limit != 0) JS_SetMemoryLimit(rt, memory_limit);
  if (stack_size != 0) JS_SetMaxStackSize(rt, stack_size);
  js_std_set_worker_new_context_func(JS_NewCustomContext);
  js_std_init_handlers(rt);
  ctx = JS_NewCustomContext(rt);
  if (!ctx) {
    fprintf(stderr, "qjs: cannot allocate JS context\n");
    exit(2);
  }

  /* loader for ES6 modules */
  JS_SetModuleLoaderFunc(rt, NULL, js_module_loader, NULL);

  if (dump_unhandled_promise_rejection) {
    JS_SetHostPromiseRejectionTracker(rt, js_std_promise_rejection_tracker,
                                      NULL);
  }

#ifdef CONFIG_BIGNUM
  if (load_jscalc) {
    js_std_eval_binary(ctx, qjsc_qjscalc, qjsc_qjscalc_size, 0);
  }
#endif
  js_std_add_helpers(ctx, -1, NULL);
  const char *str = SOURCE_CODE;
  if (eval_buf(ctx, str, strlen(str), "<quickjs.wasm>", JS_EVAL_TYPE_MODULE))
    goto fail;
  js_std_loop(ctx);

  if (dump_memory) {
    JSMemoryUsage stats;
    JS_ComputeMemoryUsage(rt, &stats);
    JS_DumpMemoryUsage(stdout, &stats, rt);
  }
  js_std_free_handlers(rt);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return 0;

fail:
  js_std_free_handlers(rt);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return 1;
}
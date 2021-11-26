VERSION=wasi-1.0.0
WASICC=wasicc
QUICKJS_ROOT=./quickjs
DIST_DIR=./dist
QUICKJS_LIB=$(QUICKJS_ROOT)/libunicode.c $(QUICKJS_ROOT)/cutils.c $(QUICKJS_ROOT)/libbf.c $(QUICKJS_ROOT)/libregexp.c $(QUICKJS_ROOT)/quickjs.c $(QUICKJS_ROOT)/quickjs-libc.c $(QUICKJS_ROOT)/qjs.c $(QUICKJS_ROOT)/repl.c $(QUICKJS_ROOT)/qjscalc.c

WASICC_FLAGS+=-D CONFIG_VERSION='"$(VERSION)"'
WASICC_FLAGS+=-D CONFIG_BIGNUM=y
# WASICC_FLAGS+=-D_WASI_EMULATED_SIGNAL
# WASICC_FLAGS+=-L -llibwasi-emulated-signal.a
WASICC_FLAGS+=-O3

install:
	git submodule update --init
	cd $(QUICKJS_ROOT); git apply ../quickjs.patch &> /dev/null || true
	cd $(QUICKJS_ROOT); make repl.c; make qjscalc.c

all: install $(DIST_DIR)/quickjs.wasm

$(DIST_DIR)/quickjs.wasm:
	mkdir -p $(DIST_DIR)
	$(WASICC) $(QUICKJS_LIB) $(WASICC_FLAGS) -o $(DIST_DIR)/quickjs.wasm

clean:
	cd $(QUICKJS_ROOT); make clean
	rm -rf $(DIST_DIR)

build:
	emcc -o quickjs.wasm \
		./quickjs/libunicode.c ./quickjs/cutils.c ./quickjs/libbf.c ./quickjs/libregexp.c ./quickjs/quickjs.c ./quickjs/quickjs-libc.c ./quickjs/qjs.c ./quickjs/repl.c ./quickjs/qjscalc.c \
		-DCONFIG_VERSION="\"1.0.0\"" -s WASM=1 -s SINGLE_FILE -s MODULARIZE=1 -lm -Oz -s EXPORTED_RUNTIME_METHODS='["ccall", "cwrap"]' --llvm-lto 3 -s AGGRESSIVE_VARIABLE_ELIMINATION=1 --closure 1 --no-entry

wasicc:
	wasicc ./quickjs/libunicode.c ./quickjs/cutils.c ./quickjs/libbf.c ./quickjs/libregexp.c ./quickjs/quickjs.c ./quickjs/quickjs-libc.c ./quickjs/qjs.c ./quickjs/repl.c ./quickjs/qjscalc.c \
		-DCONFIG_VERSION='"wasi"' \
		-D_WASI_EMULATED_SIGNAL \
		-DCONFIG_BIGNUM=y \
		-O3 \
		-o quickjs.wasm

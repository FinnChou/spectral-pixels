# Makefile - spectral-pixels
#
# The library is include/spl.h and src/spl.c (plus the generated tables src/cmf.h and
# src/srgb_lut.h); demo/ is the room shown on the web page, built on top of it.
#
# Toolchain: zig 0.17.0 (zig cc) by default; any C11 compiler works for the library, the tests and
# the native tools (make -B test CC=gcc). The WebAssembly build needs zig; `make check` needs
# Node.js 18+.
#
#   make            build docs/engine.wasm and the native program render
#   make lib        build build/libspl.a
#   make test       build and run the unit tests
#   make sanitize   run the unit tests under the address and undefined-behaviour sanitizers (clang),
#                   and check that include/spl.h compiles as C++
#   make example    build and run examples/minimal.c
#   make check      compare the native and WebAssembly builds byte by byte, and both with the
#                   reference outputs in tests/reference.txt
#   make data       regenerate src/cmf.h, src/srgb_lut.h, docs/illuminants.txt and
#                   docs/reflectances.txt from the raw data (see README.md; DATA=... for its location)
#   make format     format the C sources with clang-format
#   make clean      remove build products

CC      := zig cc
WASM_CC := zig cc
SAN_CC  := clang
CXX     := clang++
DATA    ?= data

# Floating-point contraction is disabled so that native and WebAssembly results are identical.
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -ffp-contract=off
LIB     := src/spl.c
LIB_H   := include/spl.h src/cmf.h src/srgb_lut.h
DEMO    := demo/scene.c
DEMO_H  := demo/scene.h
INC     := -Iinclude -Isrc -Idemo

all: docs/engine.wasm render

docs/engine.wasm: $(LIB) $(LIB_H) $(DEMO) $(DEMO_H) demo/wasm.c
	$(WASM_CC) $(CFLAGS) $(INC) -DSPL_SHARED -target wasm32-wasi -mexec-model=reactor -s \
		-Wl,--export-dynamic $(LIB) $(DEMO) demo/wasm.c -o $@

render: tools/render.c $(LIB) $(LIB_H) $(DEMO) $(DEMO_H)
	$(CC) $(CFLAGS) $(INC) tools/render.c $(LIB) $(DEMO) -o $@ -lm

build/spl.o: $(LIB) $(LIB_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c $(LIB) -o $@

lib: build/libspl.a
build/libspl.a: build/spl.o
	$(AR) rcs $@ $<

build/test_spl: tests/test_spl.c $(LIB) $(LIB_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_spl.c $(LIB) -o $@ -lm

test: build/test_spl
	./build/test_spl

sanitize: tests/test_spl.c $(LIB) $(LIB_H)
	@mkdir -p build
	$(SAN_CC) $(CFLAGS) -g -fsanitize=address,undefined -fno-sanitize-recover=all $(INC) \
		tests/test_spl.c $(LIB) -o build/test_spl_san -lm
	./build/test_spl_san
	$(CXX) -std=c++11 -fsyntax-only -Wall -Wextra -Wpedantic -x c++ include/spl.h

build/minimal: examples/minimal.c $(LIB) $(LIB_H)
	@mkdir -p build
	$(CC) $(CFLAGS) -Iinclude -Isrc examples/minimal.c $(LIB) -o $@ -lm

example: build/minimal
	./build/minimal build/minimal.ppm

prep: tools/prep.c
	$(CC) $(CFLAGS) tools/prep.c -o $@ -lm

data: prep
	./prep $(DATA) src docs
	cp $(DATA)/klum/LICENSE.txt docs/GPL-2.0.txt

check: all
	node tools/check.mjs

format:
	clang-format -i include/spl.h src/spl.c demo/scene.h demo/scene.c demo/wasm.c tools/render.c \
		tools/prep.c tests/test_spl.c examples/minimal.c

clean:
	rm -rf build render prep docs/engine.wasm

.PHONY: all lib test sanitize example data check format clean

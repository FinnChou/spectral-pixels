# Makefile — spectral-pixels: data preparation, native and WebAssembly builds of the engine and scene,
# and a byte-by-byte comparison of the two.
# Toolchain: zig 0.17.0 (zig cc); `make check` needs Node.js 18 or later.
#
#   make          build docs/engine.wasm and the native program render
#   make check    compare the native and WebAssembly builds byte by byte
#   make data     regenerate src/cmf.h, src/srgb_lut.h, docs/illuminants.txt and docs/reflectances.txt
#                 from the raw data (see README.md for how to obtain it; set DATA=… for its location)
#   make clean    remove the native programs

CC     := zig cc
DATA   ?= data
SRC    := src/light.c src/scene.c
HDR    := src/light.h src/scene.h src/cmf.h src/srgb_lut.h

# Both targets disable floating-point contraction (-ffp-contract=off) so that rounding is identical.
CFLAGS := -std=c11 -O2 -Wall -Wextra -ffp-contract=off

all: docs/engine.wasm render

docs/engine.wasm: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -target wasm32-wasi -mexec-model=reactor -s -Wl,--export-dynamic $(SRC) -o $@

render: tools/render.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -Isrc tools/render.c $(SRC) -o $@ -lm

prep: tools/prep.c
	$(CC) $(CFLAGS) tools/prep.c -o $@ -lm

data: prep
	./prep $(DATA) src docs
	cp $(DATA)/klum/LICENSE.txt docs/GPL-2.0.txt

check: all
	node tools/check.mjs

clean:
	rm -f render prep

.PHONY: all data check clean

/*
 * wasm.c - WebAssembly glue for the web demo
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * The module exports the engine (spl_*) and the room (scene_*) unchanged. JavaScript calls them
 * directly and passes data through buffers in the module's memory, which it obtains with
 * demo_alloc(). The memory may grow during an allocation, so JavaScript must create typed-array
 * views after allocating, not before.
 */
#include <stddef.h>
#include <stdlib.h>

#include "scene.h"

#define DEMO_EXPORT __attribute__((visibility("default")))

/* Sizes JavaScript needs: SPL_NW, SCENE_WIDTH, SCENE_HEIGHT, SCENE_ROWS, SCENE_SWATCH. */
DEMO_EXPORT const int *demo_constants(void)
{
    static const int constants[5] = { SPL_NW, SCENE_WIDTH, SCENE_HEIGHT, SCENE_ROWS, SCENE_SWATCH };
    return constants;
}

/* Zeroed buffer of size bytes in the module's memory; 0 if memory runs out. */
DEMO_EXPORT void *demo_alloc(size_t size)
{
    return calloc(1, size);
}

DEMO_EXPORT void demo_free(void *p)
{
    free(p);
}

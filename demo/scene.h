/*
 * scene.h - the demo room, a client of the spectral-pixels engine
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * The room is not part of the engine. It fills the engine's surface buffer and occluders from a
 * tile map and procedural pixel textures, writes the reflectances of its parts into the reflectance
 * table, and answers collision and visibility queries for the demo.
 */
#ifndef SPL_DEMO_SCENE_H
#define SPL_DEMO_SCENE_H

#include "spl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exported like the engine (see SPL_SHARED in spl.h); the WebAssembly build defines SPL_SHARED. */
#ifndef SCENE_API
#define SCENE_API SPL_API
#endif

/* Size of the room in pixels; the engine must be created with these dimensions. */
#define SCENE_WIDTH  256
#define SCENE_HEIGHT 160

/* Parts. Part p uses two rows of the reflectance table: 2p for its body and 2p + 1 for its joints
 * (mortar between bricks or stones). */
enum scene_part {
    PART_FLOOR,
    PART_WALL,
    PART_PILLAR,
    PART_CRATE,
    PART_BARREL,
    PART_FRAME,
    PART_COUNT
};

/* Reflectance rows the room needs. */
#define SCENE_ROWS (2 * PART_COUNT)

/* Texture styles of each part; crates, barrels and the window frame have a single style (0). */
enum floor_style {
    FLOOR_PLANKS,
    FLOOR_BRICKS,
    FLOOR_TILES,
    FLOOR_SLABS,
    FLOOR_FLAGSTONES,
    FLOOR_SPECKLED,
    FLOOR_CONCRETE,
    FLOOR_STYLES
};
enum wall_style { WALL_ASHLAR, WALL_BRICK, WALL_PLASTER, WALL_CONCRETE, WALL_STYLES };
enum pillar_style { PILLAR_PLAIN, PILLAR_SPECKLED, PILLAR_STYLES };

/* Size of a material swatch in pixels. */
#define SCENE_SWATCH 32

typedef struct scene scene;

/* Creates the room for an engine of SCENE_WIDTH × SCENE_HEIGHT pixels with at least SCENE_ROWS
 * reflectance rows; returns NULL otherwise or if memory runs out. The window starts open and every
 * part uses style 0. The engine must outlive the scene. */
SCENE_API scene *scene_create(spl_engine *engine);

/* Frees a scene; NULL is ignored. */
SCENE_API void scene_destroy(scene *sc);

/* Sets the texture style and reflectances of a part. joint may be NULL, in which case the joints
 * use the body reflectance. Call scene_build() afterwards. */
SCENE_API spl_status scene_set_material(scene *sc, int part, int style, const float body[SPL_NW],
                                        const float joint[SPL_NW]);

/* Opens (1) or closes (0, with shutters) the window in the north wall. Call scene_build()
 * afterwards. */
SCENE_API void scene_set_window(scene *sc, int open);

/* Fills the engine's surfaces, occluders and ceiling; calls spl_invalidate() if the geometry
 * changed. */
SCENE_API void scene_build(scene *sc);

/* Makes a light an area light of the given luminance filling the window opening, a uniformly bright
 * patch of sky. The light's spectrum is set separately. */
SCENE_API spl_status scene_window_light(scene *sc, int light, float luminance);

/* Screen rectangle of the window opening: x0, y0, x1, y1 in pixels, half open. The opening shows no
 * surface; the host draws the view outside into it. */
SCENE_API void scene_window_rect(int rect[4]);

/* The floor point (x, y) is occupied or outside the room. */
SCENE_API int scene_blocked(const scene *sc, float x, float y);

/* The floor point (x, y) is covered on screen by a nearer object. Valid after scene_build(). */
SCENE_API int scene_hidden(const scene *sc, float x, float y);

/* Part shown at a screen pixel; -1 in the window opening or outside the image. Valid after
 * scene_build(). */
SCENE_API int scene_part_at(const scene *sc, int x, int y);

/* SCENE_SWATCH × SCENE_SWATCH RGBA swatch of a part and style, lit by a light with spectrum spd at
 * a fixed illuminance of 1.6 times the texture shade, exposure 1. joint may be NULL. */
SCENE_API spl_status scene_swatch(int part, int style, const float spd[SPL_NW],
                                  const float body[SPL_NW], const float joint[SPL_NW],
                                  uint8_t *rgba);

#ifdef __cplusplus
}
#endif

#endif

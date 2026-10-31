/*
 * scene.h — interface of the demo scene
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * The scene is a client of the engine (light.h), not part of it: it fills the engine's input buffers
 * from a tile map and procedural pixel textures, writes the reflectances of its parts into the
 * reflectance table, and answers collision and visibility queries.
 * The map is built on first use. scene_hidden() and scene_part_at() read the image written by
 * scene_build() and must be called after it.
 */
#ifndef SPECTRAL_SCENE_H
#define SPECTRAL_SCENE_H

#include "light.h"

/* Parts. Part p uses two rows of the reflectance table: 2p for its body and 2p + 1 for its joints
 * (mortar between bricks or stones). */
enum { PART_FLOOR, PART_WALL, PART_PILLAR, PART_CRATE, PART_BARREL, PART_FRAME, PART_COUNT };

/* Texture styles of each part; crates, barrels and the window frame have a single style (0). */
enum { FLOOR_PLANKS, FLOOR_BRICKS, FLOOR_TILES, FLOOR_SLABS, FLOOR_FLAGSTONES, FLOOR_SPECKLED, FLOOR_CONCRETE };
enum { WALL_ASHLAR, WALL_BRICK, WALL_PLASTER, WALL_CONCRETE };
enum { PILLAR_PLAIN, PILLAR_SPECKLED };

/* Sets the texture style and reflectances of a part (joints use the body reflectance if joint_refl is
 * NULL). Call scene_build() afterwards. */
LIGHT_API void scene_set_material(int part, int style, const float *main_refl, const float *joint_refl);
/* Opens (1) or closes (0, with shutters) the window in the north wall. Call scene_build() afterwards. */
LIGHT_API void scene_set_window(int open);
/* Fills the engine's surface buffer and occluders; calls light_invalidate() if the geometry changed. */
LIGHT_API void scene_build(void);
/* Makes light l an area light of the given luminance filling the window opening (a uniformly bright sky). */
LIGHT_API void scene_window_light(int l, float luminance);
/* Screen rectangle of the window opening: x0, y0, x1, y1 in pixels, half open. The opening shows no
 * surface; the host draws the view outside into it. */
LIGHT_API const int *scene_window_rect(void);

/* Queries, in pixels. */
LIGHT_API int scene_blocked(float x, float y);   /* the floor point (x, y) is occupied or outside the map */
LIGHT_API int scene_hidden(float x, float y);    /* the floor point (x, y) is covered on screen by a nearer object */
LIGHT_API int scene_part_at(int sx, int sy);     /* part shown at a screen pixel; -1 in the opening or outside */

/* 32 × 32 RGBA swatch of a part and style lit by a light with spectrum spd; the illuminance is a fixed
 * 1.6 times the texture shade, the exposure 1. Calls light_surface_rgb() and so overwrites its result. */
LIGHT_API const unsigned char *scene_swatch(int part, int style, const float *spd,
                                            const float *main_refl, const float *joint_refl);

#endif

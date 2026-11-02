/*
 * scene.c - the demo room: tile map, procedural pixel textures, oblique projection and window
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * See scene.h for the interface.
 *
 * Oblique projection: the world point (x, y, z) is drawn at screen position (x, y − OBLIQUE·z).
 * Taller objects reach higher up the screen and show their face towards the viewer (+y); vertical
 * faces in other directions are not visible.
 *
 * Simplifications:
 *   - The south side (bottom of the screen) has no wall, so that the room stays visible. It is open
 *     for occlusion as well; lights should not be placed to the south.
 *   - The ceiling is as high as the walls and is not drawn.
 *   - Pixels inside the window opening are left empty for the host to draw the view outside,
 *     instead of being resolved by the projection. Strictly, a line of sight through the
 *     16-pixel-thick wall would land on the window sill first; this is a deliberate stylization.
 *     Occlusion still uses the actual thickness of the wall.
 */
#include "scene.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { TILE = SCENE_TILE, MAP_W = SCENE_MAP_W, MAP_H = SCENE_MAP_H };
_Static_assert((MAP_W * TILE) == SCENE_WIDTH && (MAP_H * TILE) == SCENE_HEIGHT,
               "the room must be a whole number of tiles");

/* Screen shift per unit of height in the oblique projection. */
#define OBLIQUE            0.5f
/* Height of the walls; the ceiling and the pillars are as high. */
#define WALL_HEIGHT        32.0f
#define PILLAR_RADIUS      6.0f
/* Height of the moulding at the base and at the top of a pillar. */
#define PILLAR_BAND        4.0f
#define BARREL_RADIUS      6.5f
/* Half the side of a crate. */
#define CRATE_HALF         7.0f
/* Distance by which points on flat faces leave the cell edge, so that occlusion tests do not start
 * inside solid space. */
#define FACE_OFFSET        0.01f
/* Distance by which points on cylinders move outwards along the normal. Cylinders are rasterized to
 * pixel cells; with a smaller offset, rays towards a light would enter a neighbouring cell of the
 * same cylinder and be counted as occluded. */
#define ROUND_OFFSET       1.0f
/* Illuminance of swatches: typical building materials (reflectance about 0.2) appear light grey. */
#define SWATCH_ILLUMINANCE 1.6f
/* Floor swatches are taken from this position, away from the origin. */
#define SWATCH_ORIGIN      40

/* Height of each part. The window frame only exists on the wall face and occupies no floor. */
static const float PART_HEIGHT[PART_COUNT] = { 0, WALL_HEIGHT, WALL_HEIGHT, 10, 12, 0 };

/* The window is in the north wall (tile row 0). The opening spans WIN_X0 <= x < WIN_X1 and
 * WIN_Z0 < z < WIN_Z1; columns WIN_MULLION and WIN_MULLION + 1 form the mullion. The frame extends
 * FRAME_W pixels sideways and FRAME_DZ vertically beyond the opening. */
enum { WIN_X0 = 96, WIN_X1 = 144, WIN_MULLION = 119, FRAME_W = 2 };
#define WIN_Z0   6.0f
#define WIN_Z1   28.0f
#define FRAME_DZ 4.0f

/* The demo room. Map: '#' wall, 'P' pillar, 'c' crate, 'o' barrel, '.' floor. */
static const char *const MAP[MAP_H] = {
    "################", "#..............#", "#..P......cc...#", "#..........c...#",
    "#....###.......#", "#......#....P..#", "#.o....#.......#", "#.oo........c..#",
    "#..............#", "#..............#",
};

struct scene {
    spl_engine *engine;
    char map[MAP_H][MAP_W];                          /* layout, one character per tile */
    unsigned char part[SCENE_HEIGHT][SCENE_WIDTH];   /* part standing on each floor point */
    float side_normal[SCENE_HEIGHT][SCENE_WIDTH][2]; /* horizontal normal of the side there */
    unsigned char round[SCENE_HEIGHT][SCENE_WIDTH];  /* the point belongs to a cylinder */
    int style[PART_COUNT];
    int window_open;
};

/* ---------- The world: which part stands on each floor point ---------- */

static void build_world(scene *sc)
{
    for (int y = 0; y < SCENE_HEIGHT; y++) {
        for (int x = 0; x < SCENE_WIDTH; x++) {
            char c = sc->map[y / TILE][x / TILE];
            float u = x % TILE + 0.5f - TILE / 2.0f;
            float v = y % TILE + 0.5f - TILE / 2.0f;
            float r = sqrtf(u * u + v * v);
            int p = PART_FLOOR;
            if (c == '#') {
                p = PART_WALL;
            } else if (c == 'P' && r < PILLAR_RADIUS) {
                p = PART_PILLAR;
            } else if (c == 'o' && r < BARREL_RADIUS) {
                p = PART_BARREL;
            } else if (c == 'c' && fabsf(u) < CRATE_HALF && fabsf(v) < CRATE_HALF) {
                p = PART_CRATE;
            }
            int round = p == PART_PILLAR || p == PART_BARREL;
            sc->part[y][x] = (unsigned char)p;
            sc->round[y][x] = (unsigned char)round;
            /* Cylinders: radial; walls and crates: +y. */
            sc->side_normal[y][x][0] = round ? u / r : 0;
            sc->side_normal[y][x][1] = round ? v / r : 1;
        }
    }
}

static float height_at(const scene *sc, int x, int y)
{
    if (x < 0 || y < 0 || x >= SCENE_WIDTH || y >= SCENE_HEIGHT) {
        return INFINITY;
    }
    return PART_HEIGHT[sc->part[y][x]];
}

/* ---------- Window ---------- */

static int window_column(int x)
{
    return x >= WIN_X0 && x < WIN_X1 && x != WIN_MULLION && x != WIN_MULLION + 1;
}

static int in_opening(int x, int y, float z)
{
    return y < TILE && window_column(x) && z > WIN_Z0 && z < WIN_Z1;
}

static int on_frame(int x, int y, float z)
{
    int inside_x = x >= WIN_X0 - FRAME_W && x < WIN_X1 + FRAME_W;
    int inside_z = z >= WIN_Z0 - FRAME_DZ && z <= WIN_Z1 + FRAME_DZ;
    return y < TILE && inside_x && inside_z && !in_opening(x, y, z);
}

/* ---------- Pseudo-random numbers for textures: integer hash and value noise. Only operations that
 * IEEE 754 defines exactly are used, so results are identical on every platform. ---------- */

static float rnd(int x, int y, int seed)
{
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) >> 8) / 16777216.0f;
}

/* Value noise: random lattice values, smoothly interpolated. */
static float noise(float x, float y, int seed)
{
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    float fx = x - ix;
    float fy = y - iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = rnd(ix, iy, seed);
    float b = rnd(ix + 1, iy, seed);
    float c = rnd(ix, iy + 1, seed);
    float d = rnd(ix + 1, iy + 1, seed);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

/* Parabolic wave of period 1 in [0, 1], used instead of sine. */
static float wave(float t)
{
    float f = t - floorf(t);
    return 4 * f * (1 - f);
}

/* Crystals in granite: scattered dark and light grains. */
static float speckle(int x, int y, int seed)
{
    float r = rnd(x, y, seed);
    float n = noise(x / 2.5f, y / 2.5f, seed + 1);
    if (r < 0.05f || n < 0.12f) {
        return 0.55f;
    }
    if (r > 0.96f) {
        return 1.25f;
    }
    return 0.92f + 0.16f * n;
}

/* ---------- Textures. joint = 1 selects the second reflectance row of the part; joints are
 * recessed and receive less light, so their shade is low. ---------- */

typedef struct {
    int joint;
    float shade;
} texel;

/* 8-pixel-wide boards in staggered rows; grain, knots and seams. */
static texel planks_texel(int x, int y)
{
    int row = y / 8;
    int off = (int)(rnd(row, 0, 1) * 64);
    int board = (x + off) / 64;
    int u = (x + off) % 64;
    int v = y % 8;
    float grain = 2 * wave(v / 3.3f + 0.64f * noise(x / 23.0f, row * 5.0f + board, 3)) - 1;
    texel t = { 0, (0.85f + 0.3f * rnd(row, board, 2)) * (0.93f + 0.07f * grain) };
    float dx = (u - 8 - rnd(row, board, 4) * 48) / 2;
    float dy = v - 3.5f;
    float d = sqrtf(dx * dx + dy * dy);
    if (rnd(row, board, 5) < 0.35f && d < 2.6f) {
        t.shade *= d < 1.2f ? 0.55f : 0.8f;
    }
    if (u == 0 || v == 0) {
        t.shade *= 0.55f;
    }
    return t;
}

/* 16 × 8 bricks in running bond, 1-pixel mortar joints. */
static texel bricks_texel(int x, int y)
{
    int row = y / 8;
    int s = x + (row % 2) * 8;
    texel t = { s % 16 == 0 || y % 8 == 0, 0 };
    if (t.joint) {
        t.shade = 0.7f + 0.1f * rnd(x, y, 6);
    } else {
        t.shade = (0.8f + 0.35f * rnd(s / 16, row, 7)) * (0.94f + 0.12f * rnd(x, y, 8));
    }
    return t;
}

/* 16 × 16 tiles, 1-pixel grout; a light and a dark edge suggest a bevel. */
static texel tiles_texel(int x, int y)
{
    int u = x % 16;
    int v = y % 16;
    texel t = { u == 0 || v == 0, 0.75f };
    if (t.joint) {
        return t;
    }
    t.shade =
        (0.82f + 0.3f * rnd(x / 16, y / 16, 9)) * (0.96f + 0.08f * noise(x / 3.0f, y / 3.0f, 10));
    if (u == 1 || v == 1) {
        t.shade *= 1.08f;
    }
    if (u == 15 || v == 15) {
        t.shade *= 0.88f;
    }
    return t;
}

/* 32 × 16 slabs in staggered rows, with fine specks. */
static texel slabs_texel(int x, int y)
{
    int row = y / 16;
    int s = x + (row % 2) * 16;
    texel t = { 0, (0.9f + 0.15f * rnd(s / 32, row, 11)) *
                       (0.93f + 0.1f * noise(x / 7.0f, y / 7.0f, 12)) };
    if (rnd(x, y, 13) < 0.02f) {
        t.shade *= 0.8f;
    }
    if (s % 32 == 0 || y % 16 == 0) {
        t.shade *= 0.6f;
    }
    return t;
}

/* Irregular flagstones: Voronoi cells with mortar between them. */
static texel flagstones_texel(int x, int y)
{
    float d1 = INFINITY;
    float d2 = INFINITY;
    int cell = 0;
    for (int b = y / 20 - 1; b <= y / 20 + 1; b++) {
        for (int a = x / 20 - 1; a <= x / 20 + 1; a++) {
            float sx = (a + 0.15f + 0.7f * rnd(a, b, 14)) * 20 - x - 0.5f;
            float sy = (b + 0.15f + 0.7f * rnd(a, b, 15)) * 20 - y - 0.5f;
            float d = sqrtf(sx * sx + sy * sy);
            if (d < d1) {
                d2 = d1;
                d1 = d;
                cell = a * 97 + b;
            } else if (d < d2) {
                d2 = d;
            }
        }
    }
    /* Nearly equidistant to the two nearest seeds: a cell border. */
    texel t = { d2 - d1 < 1.6f, 0.75f };
    if (!t.joint) {
        t.shade =
            (0.8f + 0.35f * rnd(cell, 0, 16)) * (0.92f + 0.12f * noise(x / 4.0f, y / 9.0f, 17));
    }
    return t;
}

/* Cast concrete: scattered aggregate, a joint every 64 pixels. */
static texel concrete_texel(int x, int y)
{
    texel t = { 0,
                0.9f + 0.14f * noise(x / 19.0f, y / 19.0f, 20) + 0.05f * (rnd(x, y, 21) - 0.5f) };
    if (rnd(x, y, 22) < 0.03f) {
        t.shade *= 0.75f;
    }
    if (x % 64 == 0 || y % 64 == 0) {
        t.shade *= 0.65f;
    }
    return t;
}

static texel floor_texel(int style, int x, int y)
{
    switch (style) {
    case FLOOR_PLANKS:
        return planks_texel(x, y);
    case FLOOR_BRICKS:
        return bricks_texel(x, y);
    case FLOOR_TILES:
        return tiles_texel(x, y);
    case FLOOR_SLABS:
        return slabs_texel(x, y);
    case FLOOR_FLAGSTONES:
        return flagstones_texel(x, y);
    case FLOOR_SPECKLED: {
        /* 32 × 32 granite slabs */
        texel t = { 0, 0.5f };
        if (x % 32 != 0 && y % 32 != 0) {
            t.shade = speckle(x, y, 18);
        }
        return t;
    }
    default:
        return concrete_texel(x, y);
    }
}

/* Wall face. v is the screen height above the floor (OBLIQUE·z), which keeps the masonry square on
 * screen. */
static texel wall_texel(int style, int x, float v)
{
    texel t = { 0, 1 };
    switch (style) {
    case WALL_BRICK: {
        /* Bricks 8 pixels long, courses 4 pixels high, staggered. */
        int course = (int)(v / 4);
        int s = x + (course % 2) * 4;
        t.joint = s % 8 == 0 || v - course * 4 < 0.6f;
        t.shade = t.joint ? 0.75f : 0.8f + 0.3f * rnd(course, s / 8, 30);
        break;
    }
    case WALL_PLASTER:
        /* Plaster: slight undulation and hairline cracks. */
        t.shade = 0.92f + 0.1f * noise(x / 5.0f, v / 3.0f, 31);
        if (fabsf(noise(x / 9.0f, v / 9.0f, 32) - 0.5f) < 0.015f) {
            t.shade *= 0.75f;
        }
        break;
    case WALL_CONCRETE:
        /* Fair-faced concrete: formwork joints and tie holes. */
        t.shade = 0.9f + 0.1f * noise(x / 8.0f, v / 8.0f, 33);
        if (fmodf(v, 8) < 0.6f) {
            t.shade *= 0.75f;
        }
        if (x % 16 == 8 && fabsf(fmodf(v, 8) - 4) < 0.7f) {
            t.shade *= 0.55f;
        }
        break;
    default: {
        /* Ashlar: blocks 16 pixels long, courses 4 pixels high, mortar joints. */
        int course = (int)(v / 4);
        int s = x + (course % 2) * 8;
        t.joint = s % 16 == 0 || v - course * 4 < 0.6f;
        t.shade = t.joint ? 0.75f : 0.75f + 0.25f * rnd(course, s / 16, 34);
        break;
    }
    }
    return t;
}

/* Top face of a part. a and b are coordinates within the tile, r the distance from the tile centre;
 * rim marks wall tops next to lower ground. */
static texel top_texel(int part, int style, int x, int y, int rim)
{
    int a = x % TILE;
    int b = y % TILE;
    float u = a + 0.5f - TILE / 2.0f;
    float v = b + 0.5f - TILE / 2.0f;
    float r = sqrtf(u * u + v * v);
    texel t = { 0, 1 };
    switch (part) {
    case PART_WALL:
        /* Wall top: darker along the edge so the outline reads clearly. */
        t.shade = (0.8f + 0.2f * noise(x / 3.0f, y / 3.0f, 40)) * (rim ? 0.7f : 1);
        break;
    case PART_PILLAR: {
        /* Pillar top: a moulding around the rim. */
        float base = style == PILLAR_SPECKLED ? speckle(x, y, 41) : 0.97f;
        t.shade = base * (r > PILLAR_RADIUS - 1.2f ? 0.82f : 1);
        break;
    }
    case PART_CRATE:
        /* Crate lid: frame and three boards. */
        if (a == 1 || a == TILE - 2 || b == 1 || b == TILE - 2) {
            t.shade = 0.65f;
        } else if (b == 5 || b == 10) {
            t.shade = 0.5f;
        } else {
            t.shade = 0.9f + 0.1f * noise(x / 6.0f, y * 1.3f, 42);
        }
        break;
    case PART_BARREL:
        /* Barrel lid: iron hoop and seams between staves. */
        if (r > BARREL_RADIUS - 1.2f) {
            t.shade = 0.3f;
        } else if (a == 5 || a == 10) {
            t.shade = 0.45f;
        } else {
            t.shade = 0.72f;
        }
        break;
    default:
        t = floor_texel(style, x, y);
        break;
    }
    return t;
}

/* Face of a part towards the viewer. z is the height. */
static texel side_texel(int part, int style, int x, float z)
{
    int a = x % TILE;
    texel t = { 0, 1 };
    switch (part) {
    case PART_WALL:
        return wall_texel(style, x, OBLIQUE * z);
    case PART_PILLAR: {
        /* Shaft, with a moulding at the base and at the top. */
        float base = style == PILLAR_SPECKLED ? speckle(x, (int)(z * OBLIQUE * 2), 43) : 0.97f;
        int band = z < PILLAR_BAND || z > WALL_HEIGHT - PILLAR_BAND;
        t.shade = base * (band ? 0.82f : 1);
        break;
    }
    case PART_CRATE:
        /* Crate side: posts at both ends, a top rail and one seam. */
        if (a == 1 || a == TILE - 2 || z > PART_HEIGHT[PART_CRATE] - 1.4f) {
            t.shade = 0.65f;
        } else if (fabsf(z - PART_HEIGHT[PART_CRATE] / 2) < 1) {
            t.shade = 0.5f;
        } else {
            t.shade = 0.88f + 0.1f * noise(x / 6.0f, z, 44);
        }
        break;
    case PART_BARREL:
        /* Barrel side: staves and two iron hoops. */
        if ((z > 2 && z < 3.6f) || (z > 8.4f && z < 10)) {
            t.shade = 0.3f;
        } else if (a % 3 == 0) {
            t.shade = 0.5f;
        } else {
            t.shade = 0.72f;
        }
        break;
    default:
        /* Window shutters: horizontal slats. */
        t.shade = fmodf(OBLIQUE * z, 2) < 0.6f ? 0.5f : 0.85f;
        break;
    }
    return t;
}

/* Window frame around the opening: darker at the outer columns. */
static texel frame_texel(int x)
{
    texel t = { 0, 0.8f };
    if (x == WIN_X0 - FRAME_W || x == WIN_X1 + FRAME_W - 1) {
        t.shade = 0.6f;
    }
    return t;
}

/* ---------- Occluders and projection ---------- */

static void assign(float *dst, float value, int *changed)
{
    if (*dst != value) {
        *dst = value;
        *changed = 1;
    }
}

static void fill_occluders(scene *sc, int *changed)
{
    float *heights = spl_heights(sc->engine);
    float *gaps = spl_gaps(sc->engine);
    for (int y = 0; y < SCENE_HEIGHT; y++) {
        for (int x = 0; x < SCENE_WIDTH; x++) {
            int i = y * SCENE_WIDTH + x;
            /* The opening goes through the whole thickness of the wall. */
            int hole = sc->window_open && y < TILE && window_column(x);
            assign(&heights[i], height_at(sc, x, y), changed);
            assign(&gaps[2 * i], hole ? WIN_Z0 : 0, changed);
            assign(&gaps[2 * i + 1], hole ? WIN_Z1 : 0, changed);
        }
    }
}

static void set_surface(spl_surface *s, const float p[3], const float n[3], int row, float shade,
                        int *changed)
{
    for (int c = 0; c < 3; c++) {
        assign(&s->p[c], p[c], changed);
        assign(&s->n[c], n[c], changed);
    }
    s->row = row;
    s->tex = shade;
}

/* One screen column. World cells are scanned from south (near) to north (far); nearer surfaces
 * claim the screen first and farther ones only fill what is left. The top of cell (x, y) covers
 * screen rows [top, top + 1) with top = y − OBLIQUE·h; below it, down to the rows already claimed,
 * lies its face towards +y. */
static void project_column(scene *sc, int x, int *changed)
{
    spl_surface *view = spl_surfaces(sc->engine);
    int horizon = SCENE_HEIGHT; /* rows from here down are already claimed in this column */
    for (int y = SCENE_HEIGHT - 1; y >= 0; y--) {
        int part = sc->part[y][x];
        float h = height_at(sc, x, y);
        float top = y - OBLIQUE * h;
        int sy;
        for (sy = horizon - 1; sy >= 0 && sy + 0.5f >= top; sy--) {
            float c = sy + 0.5f; /* pixel centre */
            spl_surface *s = &view[sy * SCENE_WIDTH + x];
            texel t;
            if (c < top + 1) {
                /* Top face. */
                int lower = height_at(sc, x - 1, y) < h || height_at(sc, x + 1, y) < h ||
                            height_at(sc, x, y - 1) < h || height_at(sc, x, y + 1) < h;
                int rim = part == PART_WALL && lower;
                float p[3] = { x + 0.5f, c + OBLIQUE * h, h };
                float n[3] = { 0, 0, 1 };
                t = top_texel(part, sc->style[part], x, y, rim);
                set_surface(s, p, n, 2 * part + t.joint, t.shade, changed);
                continue;
            }
            /* Face, standing on the southern edge of the cell. */
            float z = (y + 1 - c) / OBLIQUE;
            float off = sc->round[y][x] ? ROUND_OFFSET : FACE_OFFSET;
            float nx = sc->side_normal[y][x][0];
            float ny = sc->side_normal[y][x][1];
            float p[3] = { x + 0.5f + nx * off, y + 1 + ny * off, z };
            float n[3] = { nx, ny, 0 };
            int face = part;
            if (part == PART_WALL && on_frame(x, y, z)) {
                face = PART_FRAME;
            }
            if (part == PART_WALL && in_opening(x, y, z)) {
                if (sc->window_open) {
                    set_surface(s, p, n, -1, 0, changed);
                    continue;
                }
                face = PART_FRAME; /* closed window: shutters */
            }
            if (face == PART_FRAME && on_frame(x, y, z)) {
                t = frame_texel(x);
            } else {
                t = side_texel(face, sc->style[face], x, z);
            }
            set_surface(s, p, n, 2 * face + t.joint, t.shade, changed);
        }
        horizon = sy + 1;
    }
}

/* ---------- Interface ---------- */

/* The checks of spl_set_reflectance(), made before anything is written so that a rejected call
 * changes nothing. */
static int valid_reflectance(const float r[SPL_NW])
{
    for (int k = 0; k < SPL_NW; k++) {
        if (!isfinite(r[k]) || r[k] < 0) {
            return 0;
        }
    }
    return 1;
}

scene *scene_create(spl_engine *engine)
{
    spl_config cfg = spl_get_config(engine);
    if (!engine || cfg.width != SCENE_WIDTH || cfg.height != SCENE_HEIGHT ||
        cfg.rows < SCENE_ROWS) {
        return NULL;
    }
    scene *sc = calloc(1, sizeof *sc);
    if (!sc) {
        return NULL;
    }
    sc->engine = engine;
    sc->window_open = 1;
    scene_set_map(sc, NULL);
    return sc;
}

void scene_destroy(scene *sc)
{
    free(sc);
}

spl_status scene_set_material(scene *sc, int part, int style, const float body[SPL_NW],
                              const float joint[SPL_NW])
{
    if (!sc || part < 0 || part >= PART_COUNT || style < 0 || !body || !valid_reflectance(body) ||
        (joint && !valid_reflectance(joint))) {
        return SPL_ERR_ARGUMENT;
    }
    spl_status st = spl_set_reflectance(sc->engine, 2 * part, body);
    if (st != SPL_OK) {
        return st;
    }
    st = spl_set_reflectance(sc->engine, 2 * part + 1, joint ? joint : body);
    if (st != SPL_OK) {
        return st;
    }
    sc->style[part] = style;
    return SPL_OK;
}

spl_status scene_set_map(scene *sc, const char *map)
{
    if (!sc) {
        return SPL_ERR_ARGUMENT;
    }
    if (map) {
        size_t tiles = (size_t)MAP_W * MAP_H;
        if (strlen(map) != tiles || strspn(map, ".#Pco") != tiles) {
            return SPL_ERR_ARGUMENT;
        }
        memcpy(sc->map, map, sizeof sc->map);
    } else {
        for (int r = 0; r < MAP_H; r++) {
            memcpy(sc->map[r], MAP[r], MAP_W);
        }
    }
    build_world(sc);
    return SPL_OK;
}

void scene_set_window(scene *sc, int open)
{
    if (sc) {
        sc->window_open = open != 0;
    }
}

void scene_build(scene *sc)
{
    if (!sc) {
        return;
    }
    int changed = 0;
    fill_occluders(sc, &changed);
    for (int x = 0; x < SCENE_WIDTH; x++) {
        project_column(sc, x, &changed);
    }
    spl_set_ceiling(sc->engine, WALL_HEIGHT);
    if (changed) {
        spl_invalidate(sc->engine);
    }
}

spl_status scene_window_light(scene *sc, int light, float luminance)
{
    if (!sc) {
        return SPL_ERR_ARGUMENT;
    }
    /* The patch lies just outside the outer face of the north wall (y = 0), so all its points are
     * outside the grid. u is horizontal and v points down, so that u × v points into the room
     * (+y). */
    float hx = (WIN_X1 - WIN_X0) / 2.0f;
    float hz = (WIN_Z1 - WIN_Z0) / 2.0f;
    float center[3] = { WIN_X0 + hx, -FACE_OFFSET, WIN_Z0 + hz };
    float u[3] = { hx, 0, 0 };
    float v[3] = { 0, 0, -hz };
    return spl_light_set_area(sc->engine, light, center, u, v, luminance);
}

void scene_window_rect(int rect[4])
{
    rect[0] = WIN_X0;
    rect[1] = (int)(TILE - OBLIQUE * WIN_Z1);
    rect[2] = WIN_X1;
    rect[3] = (int)(TILE - OBLIQUE * WIN_Z0);
}

/* Whether (x, y) lies inside the map; false for NaN. */
static int on_map(float x, float y)
{
    return x >= 0 && y >= 0 && x < SCENE_WIDTH && y < SCENE_HEIGHT;
}

int scene_blocked(const scene *sc, float x, float y)
{
    if (!sc || !on_map(x, y)) {
        return 1;
    }
    return height_at(sc, (int)x, (int)y) > 0;
}

int scene_hidden(const scene *sc, float x, float y)
{
    /* The floor point (x, y) is drawn at screen (x, y). It is hidden if the surface shown there
     * lies further south, i.e. nearer to the viewer. */
    if (!sc || !on_map(x, y)) {
        return 0;
    }
    int sx = (int)x;
    int sy = (int)y;
    return spl_surfaces(sc->engine)[sy * SCENE_WIDTH + sx].p[1] > y + 1;
}

int scene_part_at(const scene *sc, int x, int y)
{
    if (!sc || x < 0 || y < 0 || x >= SCENE_WIDTH || y >= SCENE_HEIGHT) {
        return -1;
    }
    int row = spl_surfaces(sc->engine)[y * SCENE_WIDTH + x].row;
    return row < 0 ? -1 : row / 2;
}

/* Texel of a swatch pixel (x, y); up is the height above the swatch's bottom edge. */
static texel swatch_texel(int part, int style, int x, int y, float up)
{
    switch (part) {
    case PART_FLOOR:
        return floor_texel(style, x + SWATCH_ORIGIN, y + SWATCH_ORIGIN);
    case PART_WALL:
        return wall_texel(style, x, up);
    case PART_CRATE:
        return top_texel(PART_CRATE, style, x, y, 0);
    case PART_BARREL:
        return side_texel(PART_BARREL, style, x, up * PART_HEIGHT[PART_BARREL] / SCENE_SWATCH);
    case PART_PILLAR:
        return side_texel(PART_PILLAR, style, x, up * WALL_HEIGHT / SCENE_SWATCH);
    default:
        return side_texel(PART_FRAME, style, x, up / OBLIQUE);
    }
}

spl_status scene_swatch(int part, int style, const float spd[SPL_NW], const float body[SPL_NW],
                        const float joint[SPL_NW], uint8_t *rgba)
{
    if (part < 0 || part >= PART_COUNT || !spd || !body || !rgba) {
        return SPL_ERR_ARGUMENT;
    }
    if (!joint) {
        joint = body;
    }
    for (int y = 0; y < SCENE_SWATCH; y++) {
        for (int x = 0; x < SCENE_SWATCH; x++) {
            float up = 31.5f - y;
            texel t = swatch_texel(part, style, x, y, up);
            uint8_t *out = rgba + 4 * (y * SCENE_SWATCH + x);
            spl_status st =
                spl_surface_rgb(spd, t.joint ? joint : body, SWATCH_ILLUMINANCE * t.shade, out);
            if (st != SPL_OK) {
                return st;
            }
            out[3] = 255;
        }
    }
    return SPL_OK;
}

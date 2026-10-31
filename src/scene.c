/*
 * scene.c — the demo scene: tile map, procedural pixel textures, oblique projection and window
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * See scene.h for the interface.
 *
 * Oblique projection: the world point (x, y, z) is drawn at screen position (x, y − OBLIQUE·z). Taller
 * objects reach higher up the screen and show their face towards the viewer (+y); vertical faces in
 * other directions are not visible.
 *
 * Simplifications:
 *   - The south side (bottom of the screen) has no wall, so that the room stays visible. It is open
 *     for occlusion as well; lights should not be placed to the south.
 *   - The ceiling is as high as the walls and is not drawn.
 *   - Pixels inside the window opening are left empty for the host to draw the view outside, instead
 *     of being resolved by the projection. Strictly, a line of sight through the 16-pixel-thick wall
 *     would land on the window sill first; this is a deliberate stylization. Occlusion still uses the
 *     actual thickness of the wall.
 */
#include <math.h>
#include <string.h>
#include "scene.h"

enum { TILE = 16, MAP_W = LIGHT_W / TILE, MAP_H = LIGHT_H / TILE };
_Static_assert(MAP_W * TILE == LIGHT_W && MAP_H * TILE == LIGHT_H, "image size must be a multiple of the tile size");

#define OBLIQUE 0.5f                /* screen shift per unit of height in the oblique projection */
#define WALL_HEIGHT 32.0f           /* height of the walls; the ceiling and the pillars are as high */
#define PILLAR_RADIUS 6.0f
#define PILLAR_BAND 4.0f            /* height of the moulding at the base and top of a pillar */
#define BARREL_RADIUS 6.5f
#define CRATE_HALF 7.0f             /* half the side of a crate */
#define FACE_OFFSET 0.01f           /* distance by which points on flat faces leave the cell edge, so that
                                       occlusion tests do not start inside solid space */
#define ROUND_OFFSET 1.0f           /* distance by which points on cylinders move outwards along the normal.
                                       Cylinders are rasterized to pixel cells; with a smaller offset, rays
                                       towards a light would enter a neighbouring cell of the same cylinder
                                       and be counted as occluded */
#define SWATCH_ILLUMINANCE 1.6f     /* illuminance of swatches: typical building materials (reflectance
                                       about 0.2) appear light grey */
#define SWATCH_ORIGIN 40            /* floor swatches are taken from this position, away from the origin */

/* Height of each part. The window frame only exists on the wall face and occupies no floor. */
static const float PART_HEIGHT[PART_COUNT] = { 0, WALL_HEIGHT, WALL_HEIGHT, 10, 12, 0 };

/* The window is in the north wall (tile row 0). The opening spans WIN_X0 ≤ x < WIN_X1 and
 * WIN_Z0 < z < WIN_Z1; columns WIN_MULLION and WIN_MULLION + 1 form the mullion. The frame extends
 * FRAME_W pixels sideways and FRAME_DZ vertically beyond the opening. */
enum { WIN_X0 = 96, WIN_X1 = 144, WIN_MULLION = 119, FRAME_W = 2 };
#define WIN_Z0 6.0f
#define WIN_Z1 28.0f
#define FRAME_DZ 4.0f

/* Map: '#' wall, 'P' pillar, 'c' crate, 'o' barrel, '.' floor. */
static const char *const MAP[MAP_H] = {
  "################",
  "#..............#",
  "#..P......cc...#",
  "#..........c...#",
  "#....###.......#",
  "#......#....P..#",
  "#.o....#.......#",
  "#.oo........c..#",
  "#..............#",
  "#..............#",
};

static unsigned char part_map[LIGHT_H][LIGHT_W];  /* part standing on each floor point */
static float side_normal[LIGHT_H][LIGHT_W][2];    /* horizontal unit normal of the object's side there */
static unsigned char round_map[LIGHT_H][LIGHT_W]; /* the point belongs to a cylinder (pillar, barrel) */
static int styles[PART_COUNT];
static int window_open = 1;
static int world_ready;
static unsigned char swatch_buf[32 * 32 * 4];

static void ensure_world(void) {
  if (world_ready) return;
  for (int y = 0; y < LIGHT_H; y++)
    for (int x = 0; x < LIGHT_W; x++) {
      char c = MAP[y / TILE][x / TILE];
      float u = x % TILE + 0.5f - TILE / 2.0f, v = y % TILE + 0.5f - TILE / 2.0f, r = sqrtf(u * u + v * v);
      int p = PART_FLOOR;
      if (c == '#') p = PART_WALL;
      else if (c == 'P' && r < PILLAR_RADIUS) p = PART_PILLAR;
      else if (c == 'o' && r < BARREL_RADIUS) p = PART_BARREL;
      else if (c == 'c' && fabsf(u) < CRATE_HALF && fabsf(v) < CRATE_HALF) p = PART_CRATE;
      int round = p == PART_PILLAR || p == PART_BARREL;
      part_map[y][x] = (unsigned char)p;
      round_map[y][x] = (unsigned char)round;
      side_normal[y][x][0] = round ? u / r : 0;   /* cylinders: radial; walls and crates: +y */
      side_normal[y][x][1] = round ? v / r : 1;
    }
  world_ready = 1;
}

static float height_at(int x, int y) {
  return x < 0 || y < 0 || x >= LIGHT_W || y >= LIGHT_H ? INFINITY : PART_HEIGHT[part_map[y][x]];
}

/* ---------- Window ---------- */
static int window_column(int x) { return x >= WIN_X0 && x < WIN_X1 && x != WIN_MULLION && x != WIN_MULLION + 1; }
static int in_opening(int x, int y, float z) { return y < TILE && window_column(x) && z > WIN_Z0 && z < WIN_Z1; }
static int on_frame(int x, int y, float z) {
  return y < TILE && x >= WIN_X0 - FRAME_W && x < WIN_X1 + FRAME_W && z >= WIN_Z0 - FRAME_DZ &&
         z <= WIN_Z1 + FRAME_DZ && !in_opening(x, y, z);
}

/* ---------- Pseudo-random numbers for textures: integer hash and value noise. Only operations that
 * IEEE 754 defines exactly are used, so results are identical on every platform. ---------- */
static float rnd(int x, int y, int seed) {
  unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return (float)((h ^ (h >> 16)) >> 8) / 16777216.0f;
}

static float noise(float x, float y, int seed) {   /* value noise: random lattice values, smoothly interpolated */
  int ix = (int)floorf(x), iy = (int)floorf(y);
  float fx = x - ix, fy = y - iy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  float a = rnd(ix, iy, seed), b = rnd(ix + 1, iy, seed), c = rnd(ix, iy + 1, seed), d = rnd(ix + 1, iy + 1, seed);
  return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static float wave(float t) {                      /* parabolic wave of period 1 in [0, 1], used instead of sine */
  float f = t - floorf(t);
  return 4 * f * (1 - f);
}

static float speckle(int x, int y, int seed) {    /* crystals in granite: scattered dark and light grains */
  float r = rnd(x, y, seed), n = noise(x / 2.5f, y / 2.5f, seed + 1);
  if (r < 0.05f || n < 0.12f) return 0.55f;
  if (r > 0.96f) return 1.25f;
  return 0.92f + 0.16f * n;
}

/* ---------- Textures. joint = 1 selects the second reflectance row of the part; joints are recessed and
 * receive less light, so their shade is low. ---------- */
typedef struct { int joint; float shade; } Texel;

static Texel floor_texel(int style, int x, int y) {
  Texel t = { 0, 1 };
  switch (style) {
  case FLOOR_PLANKS: {                            /* 8-pixel-wide boards in staggered rows; grain, knots, seams */
    int row = y / 8, off = (int)(rnd(row, 0, 1) * 64), board = (x + off) / 64, u = (x + off) % 64, v = y % 8;
    float grain = 2 * wave(v / 3.3f + 0.64f * noise(x / 23.0f, row * 5.0f + board, 3)) - 1;
    t.shade = (0.85f + 0.3f * rnd(row, board, 2)) * (0.93f + 0.07f * grain);
    float dx = (u - 8 - rnd(row, board, 4) * 48) / 2, dy = v - 3.5f, d = sqrtf(dx * dx + dy * dy);
    if (rnd(row, board, 5) < 0.35f && d < 2.6f) t.shade *= d < 1.2f ? 0.55f : 0.8f;
    if (u == 0 || v == 0) t.shade *= 0.55f;
    break;
  }
  case FLOOR_BRICKS: {                            /* 16 × 8 bricks in running bond, 1-pixel mortar joints */
    int row = y / 8, s = x + (row % 2) * 8;
    t.joint = s % 16 == 0 || y % 8 == 0;
    t.shade = t.joint ? 0.7f + 0.1f * rnd(x, y, 6) : (0.8f + 0.35f * rnd(s / 16, row, 7)) * (0.94f + 0.12f * rnd(x, y, 8));
    break;
  }
  case FLOOR_TILES: {                             /* 16 × 16 tiles, 1-pixel grout; a light and a dark edge suggest a bevel */
    int u = x % 16, v = y % 16;
    t.joint = u == 0 || v == 0;
    t.shade = t.joint ? 0.75f : (0.82f + 0.3f * rnd(x / 16, y / 16, 9)) * (0.96f + 0.08f * noise(x / 3.0f, y / 3.0f, 10));
    if (!t.joint && (u == 1 || v == 1)) t.shade *= 1.08f;
    if (!t.joint && (u == 15 || v == 15)) t.shade *= 0.88f;
    break;
  }
  case FLOOR_SLABS: {                             /* 32 × 16 slabs in staggered rows, with fine specks */
    int row = y / 16, s = x + (row % 2) * 16;
    t.shade = (0.9f + 0.15f * rnd(s / 32, row, 11)) * (0.93f + 0.1f * noise(x / 7.0f, y / 7.0f, 12));
    if (rnd(x, y, 13) < 0.02f) t.shade *= 0.8f;
    if (s % 32 == 0 || y % 16 == 0) t.shade *= 0.6f;
    break;
  }
  case FLOOR_FLAGSTONES: {                        /* irregular flagstones: Voronoi cells with mortar between them */
    float d1 = INFINITY, d2 = INFINITY;
    int cell = 0;
    for (int b = y / 20 - 1; b <= y / 20 + 1; b++)
      for (int a = x / 20 - 1; a <= x / 20 + 1; a++) {
        float sx = (a + 0.15f + 0.7f * rnd(a, b, 14)) * 20 - x - 0.5f, sy = (b + 0.15f + 0.7f * rnd(a, b, 15)) * 20 - y - 0.5f;
        float d = sqrtf(sx * sx + sy * sy);
        if (d < d1) { d2 = d1; d1 = d; cell = a * 97 + b; } else if (d < d2) d2 = d;
      }
    t.joint = d2 - d1 < 1.6f;                     /* nearly equidistant to the two nearest seeds: a cell border */
    t.shade = t.joint ? 0.75f : (0.8f + 0.35f * rnd(cell, 0, 16)) * (0.92f + 0.12f * noise(x / 4.0f, y / 9.0f, 17));
    break;
  }
  case FLOOR_SPECKLED:                            /* 32 × 32 granite slabs */
    t.shade = x % 32 == 0 || y % 32 == 0 ? 0.5f : speckle(x, y, 18);
    break;
  default:                                        /* cast concrete: scattered aggregate, a joint every 64 pixels */
    t.shade = 0.9f + 0.14f * noise(x / 19.0f, y / 19.0f, 20) + 0.05f * (rnd(x, y, 21) - 0.5f);
    if (rnd(x, y, 22) < 0.03f) t.shade *= 0.75f;
    if (x % 64 == 0 || y % 64 == 0) t.shade *= 0.65f;
  }
  return t;
}

/* Wall face. v is the screen height above the floor (OBLIQUE·z), which keeps the masonry square on screen. */
static Texel wall_texel(int style, int x, float v) {
  Texel t = { 0, 1 };
  switch (style) {
  case WALL_BRICK: {                              /* bricks 8 pixels long, courses 4 pixels high, staggered */
    int course = (int)(v / 4), s = x + (course % 2) * 4;
    t.joint = s % 8 == 0 || v - course * 4 < 0.6f;
    t.shade = t.joint ? 0.75f : 0.8f + 0.3f * rnd(course, s / 8, 30);
    break;
  }
  case WALL_PLASTER:                              /* plaster: slight undulation and hairline cracks */
    t.shade = 0.92f + 0.1f * noise(x / 5.0f, v / 3.0f, 31);
    if (fabsf(noise(x / 9.0f, v / 9.0f, 32) - 0.5f) < 0.015f) t.shade *= 0.75f;
    break;
  case WALL_CONCRETE:                             /* fair-faced concrete: formwork joints and tie holes */
    t.shade = 0.9f + 0.1f * noise(x / 8.0f, v / 8.0f, 33);
    if (fmodf(v, 8) < 0.6f) t.shade *= 0.75f;
    if (x % 16 == 8 && fabsf(fmodf(v, 8) - 4) < 0.7f) t.shade *= 0.55f;
    break;
  default: {                                      /* ashlar: blocks 16 pixels long, courses 4 pixels high, mortar joints */
    int course = (int)(v / 4), s = x + (course % 2) * 8;
    t.joint = s % 16 == 0 || v - course * 4 < 0.6f;
    t.shade = t.joint ? 0.75f : 0.75f + 0.25f * rnd(course, s / 16, 34);
  }
  }
  return t;
}

/* Top face of a part. a and b are coordinates within the tile, r the distance from the tile centre;
 * rim marks wall tops next to lower ground. */
static Texel top_texel(int part, int style, int x, int y, int rim) {
  int a = x % TILE, b = y % TILE;
  float u = a + 0.5f - TILE / 2.0f, v = b + 0.5f - TILE / 2.0f, r = sqrtf(u * u + v * v);
  Texel t = { 0, 1 };
  switch (part) {
  case PART_WALL:                                 /* wall top: darker along the edge so the outline reads clearly */
    t.shade = (0.8f + 0.2f * noise(x / 3.0f, y / 3.0f, 40)) * (rim ? 0.7f : 1);
    break;
  case PART_PILLAR:                               /* pillar top: a moulding around the rim */
    t.shade = (style == PILLAR_SPECKLED ? speckle(x, y, 41) : 0.97f) * (r > PILLAR_RADIUS - 1.2f ? 0.82f : 1);
    break;
  case PART_CRATE:                                /* crate lid: frame and three boards */
    t.shade = a == 1 || a == TILE - 2 || b == 1 || b == TILE - 2 ? 0.65f
            : b == 5 || b == 10                              ? 0.5f
                                                             : 0.9f + 0.1f * noise(x / 6.0f, y * 1.3f, 42);
    break;
  case PART_BARREL:                               /* barrel lid: iron hoop and seams between staves */
    t.shade = r > BARREL_RADIUS - 1.2f ? 0.3f : a == 5 || a == 10 ? 0.45f : 0.72f;
    break;
  default:
    t = floor_texel(style, x, y);
  }
  return t;
}

/* Face of a part towards the viewer. z is the height. */
static Texel side_texel(int part, int style, int x, float z) {
  int a = x % TILE;
  Texel t = { 0, 1 };
  switch (part) {
  case PART_WALL:
    return wall_texel(style, x, OBLIQUE * z);
  case PART_PILLAR:                               /* shaft, with a moulding at the base and at the top */
    t.shade = (style == PILLAR_SPECKLED ? speckle(x, (int)(z * OBLIQUE * 2), 43) : 0.97f) *
              (z < PILLAR_BAND || z > WALL_HEIGHT - PILLAR_BAND ? 0.82f : 1);
    break;
  case PART_CRATE:                                /* crate side: posts at both ends, a top rail and one seam */
    t.shade = a == 1 || a == TILE - 2 || z > PART_HEIGHT[PART_CRATE] - 1.4f ? 0.65f
            : fabsf(z - PART_HEIGHT[PART_CRATE] / 2) < 1                ? 0.5f
                                                                         : 0.88f + 0.1f * noise(x / 6.0f, z, 44);
    break;
  case PART_BARREL:                               /* barrel side: staves and two iron hoops */
    t.shade = (z > 2 && z < 3.6f) || (z > 8.4f && z < 10) ? 0.3f : a % 3 == 0 ? 0.5f : 0.72f;
    break;
  default:                                        /* window shutters: horizontal slats */
    t.shade = fmodf(OBLIQUE * z, 2) < 0.6f ? 0.5f : 0.85f;
  }
  return t;
}

/* ---------- Occluders and projection ---------- */
static void assign(float *dst, float value, int *changed) {
  if (*dst != value) { *dst = value; *changed = 1; }
}

static void fill_occluders(int *changed) {
  float *heights = light_heights(), *gaps = light_gaps();
  for (int y = 0; y < LIGHT_H; y++)
    for (int x = 0; x < LIGHT_W; x++) {
      int i = y * LIGHT_W + x, hole = window_open && y < TILE && window_column(x);  /* through the whole wall */
      assign(&heights[i], height_at(x, y), changed);
      assign(&gaps[2 * i], hole ? WIN_Z0 : 0, changed);
      assign(&gaps[2 * i + 1], hole ? WIN_Z1 : 0, changed);
    }
}

static void set_surface(LightSurface *s, const float p[3], const float n[3], int row, float shade, int *changed) {
  for (int c = 0; c < 3; c++) { assign(&s->p[c], p[c], changed); assign(&s->n[c], n[c], changed); }
  s->row = row;
  s->tex = shade;
}

/* One screen column. World cells are scanned from south (near) to north (far); nearer surfaces claim the
 * screen first and farther ones only fill what is left. The top of cell (x, y) covers screen rows
 * [top, top + 1) with top = y − OBLIQUE·h; below it, down to the rows already claimed, lies its face
 * towards +y. */
static void project_column(int x, int *changed) {
  LightSurface *view = light_surfaces();
  int horizon = LIGHT_H;                          /* rows from here down are already claimed in this column */
  for (int y = LIGHT_H - 1; y >= 0; y--) {
    int part = part_map[y][x];
    float h = height_at(x, y), top = y - OBLIQUE * h;
    int sy;
    for (sy = horizon - 1; sy >= 0 && sy + 0.5f >= top; sy--) {
      float c = sy + 0.5f;                        /* pixel centre */
      LightSurface *s = &view[sy * LIGHT_W + x];
      Texel t;
      if (c < top + 1) {                          /* top face */
        int rim = part == PART_WALL && (height_at(x - 1, y) < h || height_at(x + 1, y) < h ||
                                        height_at(x, y - 1) < h || height_at(x, y + 1) < h);
        float p[3] = { x + 0.5f, c + OBLIQUE * h, h }, n[3] = { 0, 0, 1 };
        t = top_texel(part, styles[part], x, y, rim);
        set_surface(s, p, n, 2 * part + t.joint, t.shade, changed);
      } else {                                    /* face, standing on the southern edge of the cell */
        float z = (y + 1 - c) / OBLIQUE, off = round_map[y][x] ? ROUND_OFFSET : FACE_OFFSET;
        float nx = side_normal[y][x][0], ny = side_normal[y][x][1];
        float p[3] = { x + 0.5f + nx * off, y + 1 + ny * off, z }, n[3] = { nx, ny, 0 };
        int face = part;
        if (part == PART_WALL && on_frame(x, y, z)) face = PART_FRAME;
        if (part == PART_WALL && in_opening(x, y, z)) {
          if (window_open) { set_surface(s, p, n, -1, 0, changed); continue; }
          face = PART_FRAME;                      /* closed window: shutters */
        }
        t = face == PART_FRAME && on_frame(x, y, z)
              ? (Texel){ 0, x == WIN_X0 - FRAME_W || x == WIN_X1 + FRAME_W - 1 ? 0.6f : 0.8f }
              : side_texel(face, styles[face], x, z);
        set_surface(s, p, n, 2 * face + t.joint, t.shade, changed);
      }
    }
    horizon = sy + 1;
  }
}

/* ---------- Interface ---------- */
LIGHT_API void scene_set_material(int part, int style, const float *main_refl, const float *joint_refl) {
  if (part < 0 || part >= PART_COUNT) return;
  styles[part] = style;
  light_set_reflectance(2 * part, main_refl);
  light_set_reflectance(2 * part + 1, joint_refl ? joint_refl : main_refl);
}

LIGHT_API void scene_set_window(int open) { window_open = open != 0; }

LIGHT_API void scene_build(void) {
  int changed = 0;
  ensure_world();
  fill_occluders(&changed);
  for (int x = 0; x < LIGHT_W; x++) project_column(x, &changed);
  light_set_ceiling(WALL_HEIGHT);
  if (changed) light_invalidate();
}

LIGHT_API void scene_window_light(int l, float luminance) {
  /* The patch lies just outside the outer face of the north wall (y = 0), so all its points are outside
   * the grid. u is horizontal and v points down, so that u × v points into the room (+y). */
  float hx = (WIN_X1 - WIN_X0) / 2.0f, hz = (WIN_Z1 - WIN_Z0) / 2.0f;
  light_area(l, WIN_X0 + hx, -FACE_OFFSET, WIN_Z0 + hz, hx, 0, 0, 0, 0, -hz, luminance);
}

LIGHT_API const int *scene_window_rect(void) {
  static const int rect[4] = { WIN_X0, (int)(TILE - OBLIQUE * WIN_Z1), WIN_X1, (int)(TILE - OBLIQUE * WIN_Z0) };
  return rect;
}

LIGHT_API int scene_blocked(float x, float y) {
  ensure_world();
  return height_at((int)floorf(x), (int)floorf(y)) > 0;
}

LIGHT_API int scene_hidden(float x, float y) {
  /* The floor point (x, y) is drawn at screen (x, y). It is hidden if the surface shown there lies further
   * south, i.e. nearer to the viewer. */
  int sx = (int)floorf(x), sy = (int)floorf(y);
  if (sx < 0 || sy < 0 || sx >= LIGHT_W || sy >= LIGHT_H) return 0;
  return light_surfaces()[sy * LIGHT_W + sx].p[1] > y + 1;
}

LIGHT_API int scene_part_at(int sx, int sy) {
  int row = light_row_at(sx, sy);
  return row < 0 ? -1 : row / 2;
}

LIGHT_API const unsigned char *scene_swatch(int part, int style, const float *spd,
                                            const float *main_refl, const float *joint_refl) {
  if (!joint_refl) joint_refl = main_refl;
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      float up = 31.5f - y;                       /* height above the bottom edge, for face textures */
      Texel t = part == PART_FLOOR  ? floor_texel(style, x + SWATCH_ORIGIN, y + SWATCH_ORIGIN)
              : part == PART_WALL   ? wall_texel(style, x, up)
              : part == PART_CRATE  ? top_texel(PART_CRATE, style, x, y, 0)
              : part == PART_BARREL ? side_texel(PART_BARREL, style, x, up * PART_HEIGHT[PART_BARREL] / 32)
              : part == PART_PILLAR ? side_texel(PART_PILLAR, style, x, up * WALL_HEIGHT / 32)
                                    : side_texel(PART_FRAME, style, x, up / OBLIQUE);
      const unsigned char *c = light_surface_rgb(spd, t.joint ? joint_refl : main_refl, SWATCH_ILLUMINANCE * t.shade);
      unsigned char *out = swatch_buf + 4 * (y * 32 + x);
      memcpy(out, c, 3);
      out[3] = 255;
    }
  return swatch_buf;
}

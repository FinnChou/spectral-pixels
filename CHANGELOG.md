# Changelog

All notable changes to this project are recorded here. Versions follow
[Semantic Versioning](https://semver.org/); while the major version is 0, a minor version may change the interface.

## [Unreleased]

### Added

- Demo: `scene_set_map()` lays out the room from a tile map (`SCENE_MAP_W` × `SCENE_MAP_H` characters), so that a
  caller can build simpler scenes, for example a bare floor or a floor with a few objects. `NULL` restores the demo
  room, whose output is unchanged. `render` takes the layout as `map=`, and `make check` covers a custom layout.

## [0.1.0] - 2026-11-01

First versioned release. The engine becomes a library that other programs can use directly. It replaces the
unversioned interface of the initial commit (`src/light.h`, prefix `light_`).

### Changed

- The public header is `include/spl.h`; every function and type has the prefix `spl_`.
- All state lives in an `spl_engine`, created with `spl_create(&config, &engine)` and freed with `spl_destroy()`.
  The image size, the number of reflectance rows and the number of lights are chosen at run time through
  `spl_config`, up to `SPL_MAX_SIZE`, `SPL_MAX_ROWS` and `SPL_MAX_LIGHTS`; they were fixed at compile time before
  (`LIGHT_W`, `LIGHT_H`, `LIGHT_ROWS`, `LIGHT_MAX`). The library has no global state.
- Functions that can fail return `spl_status` (`SPL_OK`, `SPL_ERR_ARGUMENT`, `SPL_ERR_MEMORY`) and validate their
  arguments.
- `spl_render()`, `spl_probe()`, `spl_light_rgb()` and `spl_surface_rgb()` write into buffers of the caller instead
  of returning pointers to internal static buffers.
- Renamed:

  | Before | Now |
  |---|---|
  | `light_surfaces`, `light_heights`, `light_gaps`, `light_invalidate` | `spl_surfaces`, `spl_heights`, `spl_gaps`, `spl_invalidate` |
  | `light_set_ceiling` | `spl_set_ceiling`, which returns a status |
  | `light_set_spectrum` | `spl_light_set_spectrum` |
  | `light_point`, `light_sun`, `light_area`, `light_off` | `spl_light_set_point`, `spl_light_set_directional`, `spl_light_set_area` (center and half-edges as arrays), `spl_light_disable` |
  | `light_set_reflectance`, `light_set_ambient`, `light_set_exposure` | `spl_set_reflectance`, `spl_set_ambient`, `spl_set_exposure` |
  | `light_render`, `light_probe` | `spl_render`, `spl_probe` |
  | `light_source_rgb`, `light_surface_rgb` | `spl_light_rgb`, `spl_surface_rgb` |
  | `LightSurface` (with `int row`) | `spl_surface` (with `int32_t row`) |
  | `LIGHT_NW`, `LIGHT_API` | `SPL_NW`, `SPL_API` |

- Removed: `light_row_at` (read `row` from `spl_surfaces()`), `light_scratch`, `light_width`, `light_height`,
  `light_spectrum_size` and `light_max` (use `spl_get_config()` and `SPL_NW`).
- `SPL_API` exports symbols only when `SPL_SHARED` is defined, so that a program that links the library statically,
  or builds it into its own shared library with hidden visibility, does not export them.
- The demo scene moved from `src/` to `demo/` and uses the engine only through `spl.h`; its functions take a `scene`
  created with `scene_create()`. The WebAssembly build exports `demo_alloc`, `demo_free` and `demo_constants` for the
  page.
- The data files carry Japanese names and attributions in addition to English and Chinese; the demo interface is in
  Chinese, English and Japanese.

### Changed behaviour for invalid input

- A rejected call leaves the engine unchanged. Before, a zero direction (`light_sun`), a degenerate area light
  (`light_area`) or a spectrum with zero luminance (`light_set_spectrum`) switched the light off; now the call
  returns `SPL_ERR_ARGUMENT` and the light keeps shining as before.
- A negative ambient factor or exposure is rejected instead of being clamped to 0.
- Probing a pixel outside the image returns `SPL_ERR_ARGUMENT` instead of zeros.
- `spl_light_rgb()` and `spl_surface_rgb()` reject the spectra that `spl_light_set_spectrum()` rejects.

### Fixed

- Undefined behaviour with a light billions of pixels away, or with a surface position that is not finite.
- Out-of-bounds writes with a reflectance table of more than about 35 million rows (now limited by `SPL_MAX_ROWS`).
- Pixels so bright that tone mapping overflowed (above about 10¹⁹) were drawn black; they are now white.
- The documentation of `spl_heights()`: every point below the height is solid, including points below z = 0.

### Added

- Version macros `SPL_VERSION_MAJOR`, `SPL_VERSION_MINOR`, `SPL_VERSION_PATCH` and `spl_version()`.
- `spl_get_config()`, the limits `SPL_MAX_*`, and the sampling constants `SPL_LAMBDA_MIN` and `SPL_LAMBDA_STEP`.
- Unit tests (`tests/test_spl.c`; `make test`, and `make sanitize` under the address and undefined-behaviour
  sanitizers), a minimal example (`examples/minimal.c`, `make example`), a static library target (`make lib`) and a
  clang-format configuration (`make format`).
- `make check` also compares the outputs with the reference hashes in `tests/reference.txt`, so that a change of
  output is noticed even when the native and WebAssembly builds change together.
- CI runs the unit tests with zig cc, gcc and clang and under the sanitizers, and builds the library.

### Unchanged

- For valid input the arithmetic is the same. Rendered images, probe spectra and the colours of the demo are
  byte-identical to those of the previous implementation; `tests/reference.txt` was recorded from it.

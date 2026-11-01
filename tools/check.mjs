// check.mjs — byte-by-byte comparison of the native build (render) and the WebAssembly build (engine.wasm)
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 FinnZhou
//
// Both builds read the same spectral data and render the same settings; every byte of the RGBA output is
// compared, as are the incident and reflected spectra at a few pixels (float by float). The WebAssembly
// instance is reused across cases to exercise the invalidation of the geometry cache; the native program
// runs once per case and so starts from an empty cache. The two must agree exactly. Usage: make check
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = fileURLToPath(new URL('.', import.meta.url));
const ROOT = join(HERE, '..'), DOCS = join(ROOT, 'docs');
const DATA = [join(DOCS, 'illuminants.txt'), join(DOCS, 'reflectances.txt')];
const PARTS = ['floor', 'wall', 'pillar', 'crate', 'barrel', 'frame'];   // same order as PART_* in scene.h

const { instance } = await WebAssembly.instantiate(readFileSync(join(DOCS, 'engine.wasm')), {});
const E = instance.exports;
E._initialize();
const W = E.light_width(), H = E.light_height(), NW = E.light_spectrum_size(), LIGHT_MAX = E.light_max();

const spectra = {};
for (const file of DATA)
  for (const line of readFileSync(file, 'utf8').split('\n')) {
    if (!line || line.startsWith('#')) continue;
    const field = line.split('\t');                // id, English, Chinese and Japanese names, values
    if (field.length !== 4 + NW || field.slice(4).some((v) => v === '' || !Number.isFinite(+v))) throw new Error(`malformed line: ${field[0]}`);
    spectra[field[0]] = Float32Array.from(field.slice(4), Number);
  }

function put(slot, id) {                        // copies a spectrum into the engine's scratch area; returns its address
  const at = E.light_scratch() + slot * NW * 4;
  new Float32Array(E.memory.buffer, at, NW).set(spectra[id]);
  return at;
}

function renderWasm(c) {
  PARTS.forEach((name, part) => {
    const [main, joint, style] = c.parts[name];
    E.scene_set_material(part, style, put(0, main), joint === '-' ? 0 : put(1, joint));
  });
  E.scene_set_window(c.window);
  E.scene_build();
  for (let l = 0; l < LIGHT_MAX; l++) {
    const light = c.lights[l];
    if (!light) { E.light_off(l); continue; }
    const [kind, id, ...v] = light;
    E.light_set_spectrum(l, put(0, id));
    if (kind === 'point') E.light_point(l, ...v);
    else if (kind === 'sun') E.light_sun(l, ...v);
    else if (kind === 'area') E.light_area(l, ...v);
    else E.scene_window_light(l, ...v);
  }
  E.light_set_ambient(c.ambient);
  E.light_set_exposure(c.exposure);
  const rgba = new Uint8Array(E.memory.buffer, E.light_render(), W * H * 4).slice();
  const probes = c.probes.map(([x, y]) => new Float32Array(E.memory.buffer, E.light_probe(x, y), 2 * NW).slice());
  return { rgba, probes };
}

function renderNative(c, dir) {
  const args = [
    ...PARTS.map((name) => `${name}=${c.parts[name].join(',')}`),
    ...c.lights.map((light, l) => `light${l}=${light.join(',')}`),
    `window=${c.window}`, `ambient=${c.ambient}`, `exposure=${c.exposure}`,
    ...c.probes.map(([x, y]) => `probe=${x},${y}`),
  ];
  const image = join(dir, 'frame.pam');
  const stdout = execFileSync(join(ROOT, 'render'), [...DATA, image, ...args], { encoding: 'utf8' });
  const pam = readFileSync(image), header = pam.indexOf('ENDHDR\n') + 'ENDHDR\n'.length;
  const probes = stdout.trim().split('\n').filter(Boolean).map((line) => Float32Array.from(line.split(' '), Number));
  return { rgba: pam.subarray(header), probes };
}

// Cases. A light is [kind, spectrum id, parameters…] as on the render command line; a part is
// [body, joint, style], where joint - means the same as body.
const lamp = (id, x, y) => ['point', id, x, y, 18, 2000];
const base = {
  parts: {
    floor: ['KLUM-B002', 'KLUM-C001', 1], wall: ['KLUM-G001', 'KLUM-C001', 0], pillar: ['KLUM-G002', '-', 0],
    crate: ['KLUM-L002', '-', 0], barrel: ['KLUM-L002', '-', 0], frame: ['KLUM-L001', '-', 0],
  },
  window: 1, ambient: 1, exposure: 1, probes: [[88, 104], [56, 40], [40, 99], [120, 8]],
};
const sunny = [lamp('planck-1900', 88, 104), ['sun', 'cie-d55', 0.35, -0.85, 0.45, 6]];
const cases = [
  { ...base, lights: sunny },
  // same lights and geometry, only styles and reflectances change: the cached geometry must be reused
  { ...base, lights: sunny, parts: { ...base.parts, floor: ['KLUM-L002', '-', 0], wall: ['KLUM-B002', 'KLUM-C001', 1] } },
  // window closed; lamp at a fractional position; exposure other than 1
  { ...base, lights: [lamp('cie-hp1', 40.3, 30.7)], window: 0, exposure: 1.5 },
  // the window opening as an area light; granite pillars
  { ...base, lights: [lamp('cie-led-rgb1', 150.25, 60.5), ['window', 'cie-d75', 6]],
    parts: { ...base.parts, floor: ['KLUM-J106', 'KLUM-C001', 4], wall: ['KLUM-I101', '-', 2], pillar: ['KLUM-F103', '-', 1] } },
  // a point light outside the window
  { ...base, lights: [lamp('cie-fl7', 230, 140), ['point', 'cie-hp1', 132, -40, 44, 30000]],
    parts: { ...base.parts, floor: ['KLUM-F103', '-', 5], wall: ['KLUM-E301', '-', 3], frame: ['KLUM-L102', '-', 0] } },
  // an area light with an arbitrary orientation; no ambient light
  { ...base, lights: [lamp('cie-led-b5', 120.6, 50.2), ['area', 'cie-daylight-10000', 128, 80, 31, 12, 0, 0, 0, 6, -2, 3]],
    ambient: 0, parts: { ...base.parts, floor: ['KLUM-E405', '-', 6], wall: ['KLUM-B103', 'KLUM-C001', 1] } },
];

const dir = mkdtempSync(join(tmpdir(), 'spectral-check-'));
let differ = 0, worst = 0, probeDiffer = 0;
try {
  for (const c of cases) {
    const wasm = renderWasm(c), native = renderNative(c, dir);
    if (native.rgba.length !== wasm.rgba.length) throw new Error('image sizes differ');
    for (let i = 0; i < wasm.rgba.length; i++) {
      const d = Math.abs(native.rgba[i] - wasm.rgba[i]);
      if (d) differ++;
      worst = Math.max(worst, d);
    }
    wasm.probes.forEach((p, j) => p.forEach((v, k) => { if (v !== native.probes[j][k]) probeDiffer++; }));
  }
} finally {
  rmSync(dir, { recursive: true, force: true });
}
console.log(`${cases.length} frames × ${W}×${H} pixels × RGBA: ${differ} bytes differ, largest difference ${worst}; ` +
            `${cases.length * base.probes.length} probes: ${probeDiffer} values differ`);
process.exit(differ === 0 && probeDiffer === 0 ? 0 : 1);

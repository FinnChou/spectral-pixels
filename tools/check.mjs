// check.mjs — byte-by-byte comparison of the native build (render) and the WebAssembly build (engine.wasm),
// and of both with the reference outputs in tests/reference.txt
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 FinnZhou
//
// Both builds read the same spectral data and render the same settings; every byte of the RGBA output is
// compared, as are the incident and reflected spectra at a few pixels (float by float). The WebAssembly
// instance is reused across cases to exercise the invalidation of the geometry cache; the native program
// runs once per case and so starts from an empty cache. The two must agree exactly.
//
// The frames and probes, and the colours the demo draws with (material swatches, light colours, surface
// colours), are then hashed and compared with tests/reference.txt, so that a change of output is noticed
// even when both builds change together. After an intended change, regenerate the reference with
// `node tools/check.mjs --update` and explain the change in the commit.
//
// Usage: make check
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = fileURLToPath(new URL('.', import.meta.url));
const ROOT = join(HERE, '..'), DOCS = join(ROOT, 'docs');
const DATA = [join(DOCS, 'illuminants.txt'), join(DOCS, 'reflectances.txt')];
const REFERENCE = join(ROOT, 'tests', 'reference.txt');
const UPDATE = process.argv.includes('--update');
const PARTS = ['floor', 'wall', 'pillar', 'crate', 'barrel', 'frame'];   // same order as PART_* in scene.h

const { instance } = await WebAssembly.instantiate(readFileSync(join(DOCS, 'engine.wasm')), {});
const E = instance.exports;
E._initialize();
const [NW, W, H, ROWS] = new Int32Array(E.memory.buffer, E.demo_constants(), 5);
const LIGHTS = 8;

// Engine and room, created through the same exports the web demo uses. Buffers are allocated once, before
// any typed-array view is created, because an allocation may grow the memory.
const config = E.demo_alloc(16), spectrumBuf = E.demo_alloc(3 * NW * 4), vectors = E.demo_alloc(9 * 4);
const frame = E.demo_alloc(W * H * 4), probeBuf = E.demo_alloc(2 * NW * 4), created = E.demo_alloc(4);
const MAP_TILES = 16 * 10, mapBuf = E.demo_alloc(MAP_TILES + 1);
const SWATCH = new Int32Array(E.memory.buffer, E.demo_constants(), 5)[4];
const swatchBuf = E.demo_alloc(SWATCH * SWATCH * 4), rgbBuf = E.demo_alloc(4);
const ok = (status, what) => { if (status !== 0) throw new Error(`${what} failed (${status})`); };
new Int32Array(E.memory.buffer, config, 4).set([W, H, ROWS, LIGHTS]);
ok(E.spl_create(config, created), 'spl_create');
const engine = new Uint32Array(E.memory.buffer, created, 1)[0], room = E.scene_create(engine);
if (!room) throw new Error('could not create the room');

const spectra = {};
for (const file of DATA)
  for (const line of readFileSync(file, 'utf8').split('\n')) {
    if (!line || line.startsWith('#')) continue;
    const field = line.split('\t');                // id, English, Chinese and Japanese names, values
    if (field.length !== 4 + NW || field.slice(4).some((v) => v === '' || !Number.isFinite(+v))) throw new Error(`malformed line: ${field[0]}`);
    spectra[field[0]] = Float32Array.from(field.slice(4), Number);
  }

function put(slot, id) {                        // copies a spectrum into slot 0, 1 or 2 of spectrumBuf; returns its address
  const at = spectrumBuf + slot * NW * 4;
  new Float32Array(E.memory.buffer, at, NW).set(spectra[id]);
  return at;
}

function renderWasm(c) {
  PARTS.forEach((name, part) => {
    const [main, joint, style] = c.parts[name];
    ok(E.scene_set_material(room, part, style, put(0, main), joint === '-' ? 0 : put(1, joint)), name);
  });
  if (c.map) new Uint8Array(E.memory.buffer, mapBuf, MAP_TILES + 1).set([...new TextEncoder().encode(c.map), 0]);
  ok(E.scene_set_map(room, c.map ? mapBuf : 0), 'map');
  E.scene_set_window(room, c.window);
  E.scene_build(room);
  for (let l = 0; l < LIGHTS; l++) {
    const light = c.lights[l];
    if (!light) { ok(E.spl_light_disable(engine, l), 'disable'); continue; }
    const [kind, id, ...v] = light;
    ok(E.spl_light_set_spectrum(engine, l, put(0, id)), id);
    if (kind === 'point') ok(E.spl_light_set_point(engine, l, ...v), kind);
    else if (kind === 'sun') ok(E.spl_light_set_directional(engine, l, ...v), kind);
    else if (kind === 'area') {
      new Float32Array(E.memory.buffer, vectors, 9).set(v.slice(0, 9));
      ok(E.spl_light_set_area(engine, l, vectors, vectors + 12, vectors + 24, v[9]), kind);
    } else ok(E.scene_window_light(room, l, ...v), kind);
  }
  ok(E.spl_set_ambient(engine, c.ambient), 'ambient');
  ok(E.spl_set_exposure(engine, c.exposure), 'exposure');
  ok(E.spl_render(engine, frame), 'render');
  const rgba = new Uint8Array(E.memory.buffer, frame, W * H * 4).slice();
  const probes = c.probes.map(([x, y]) => {
    ok(E.spl_probe(engine, x, y, probeBuf, probeBuf + NW * 4), 'probe');
    return new Float32Array(E.memory.buffer, probeBuf, 2 * NW).slice();
  });
  return { rgba, probes };
}

function renderNative(c, dir) {
  const args = [
    ...PARTS.map((name) => `${name}=${c.parts[name].join(',')}`),
    ...c.lights.map((light, l) => `light${l}=${light.join(',')}`),
    ...(c.map ? [`map=${c.map}`] : []), `window=${c.window}`, `ambient=${c.ambient}`, `exposure=${c.exposure}`,
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
  // a room laid out with scene_set_map(): no walls, a few objects, a lamp and the sun
  { ...base, map: '................' + '..c.........P...' + '................' + '.....o..........'.repeat(2) +
                  '................'.repeat(5), lights: [lamp('cie-a', 60.5, 70.5), ['sun', 'cie-d65', -0.3, 0.4, 0.8, 5]] },
];

// The colours the demo draws with: swatches of every part and style under D65 for three materials, the colour
// of every light, and surface colours under two lights at three illuminances.
const STYLES = [7, 4, 2, 1, 1, 1];             // FLOOR_STYLES, WALL_STYLES, PILLAR_STYLES and one style for the rest
function colours() {
  const hash = createHash('sha256');
  for (let part = 0; part < PARTS.length; part++)
    for (let style = 0; style < STYLES[part]; style++)
      for (const [body, joint] of [['KLUM-B002', 'KLUM-C001'], ['KLUM-L002', null], ['KLUM-G001', 'KLUM-C001']]) {
        ok(E.scene_swatch(part, style, put(0, 'cie-d65'), put(1, body), joint ? put(2, joint) : 0, swatchBuf), 'swatch');
        hash.update(new Uint8Array(E.memory.buffer, swatchBuf, SWATCH * SWATCH * 4));
      }
  for (const id of Object.keys(spectra).filter((k) => !k.startsWith('KLUM'))) {
    ok(E.spl_light_rgb(put(0, id), rgbBuf), id);
    hash.update(new Uint8Array(E.memory.buffer, rgbBuf, 3));
  }
  for (const id of ['planck-1900', 'cie-hp1'])
    for (const body of ['KLUM-B002', 'KLUM-D301'])
      for (const e of [0.1, 1, 5]) {
        ok(E.spl_surface_rgb(put(0, id), put(1, body), e, rgbBuf), 'surface colour');
        hash.update(new Uint8Array(E.memory.buffer, rgbBuf, 3));
      }
  return hash.digest('hex');
}

const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');
const dir = mkdtempSync(join(tmpdir(), 'spectral-check-'));
const outputs = [];
let differ = 0, worst = 0, probeDiffer = 0;
try {
  cases.forEach((c, n) => {
    const wasm = renderWasm(c), native = renderNative(c, dir);
    if (native.rgba.length !== wasm.rgba.length) throw new Error('image sizes differ');
    for (let i = 0; i < wasm.rgba.length; i++) {
      const d = Math.abs(native.rgba[i] - wasm.rgba[i]);
      if (d) differ++;
      worst = Math.max(worst, d);
    }
    wasm.probes.forEach((p, j) => p.forEach((v, k) => { if (v !== native.probes[j][k]) probeDiffer++; }));
    const probes = new Float32Array(wasm.probes.length * 2 * NW);
    wasm.probes.forEach((p, j) => probes.set(p, j * 2 * NW));
    outputs.push([`case${n + 1}-rgba`, sha256(wasm.rgba)], [`case${n + 1}-probes`, sha256(new Uint8Array(probes.buffer))]);
  });
} finally {
  rmSync(dir, { recursive: true, force: true });
}
outputs.push(['colours', colours()]);
console.log(`${cases.length} frames × ${W}×${H} pixels × RGBA: ${differ} bytes differ, largest difference ${worst}; ` +
            `${cases.length * base.probes.length} probes: ${probeDiffer} values differ`);

if (UPDATE) {
  writeFileSync(REFERENCE, '# SHA-256 of the outputs checked by tools/check.mjs. Regenerate with `node tools/check.mjs --update`\n' +
                           '# only for an intended change of output.\n' + outputs.map(([k, v]) => `${k} ${v}\n`).join(''));
  console.log(`wrote ${REFERENCE}`);
}
const reference = new Map(readFileSync(REFERENCE, 'utf8').split('\n').filter((l) => l && !l.startsWith('#')).map((l) => l.split(' ')));
const changed = outputs.filter(([k, v]) => reference.get(k) !== v).map(([k]) => k);
if (reference.size !== outputs.length) changed.push('(number of entries)');
console.log(`reference: ${outputs.length} outputs, ${changed.length ? 'changed: ' + changed.join(', ') : 'all unchanged'}`);
process.exit(differ === 0 && probeDiffer === 0 && changed.length === 0 ? 0 : 1);

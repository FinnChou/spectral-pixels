/*
 * demo.js — user interface and interaction of the spectral lighting demo
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * All lighting is computed by engine.wasm (built from src/spl.c, the engine, and demo/scene.c, the room;
 * see include/spl.h and demo/scene.h). This file only
 * loads the spectral data and hands the selected spectra to the engine, handles keyboard and pointer
 * input, and draws the frame, the view outside the window and the spectral charts.
 *
 * Usage in a page:
 *   <link rel="stylesheet" href="<base>/demo.css">
 *   <div class="spl-demo" data-base="<base>/"></div>
 *   <script src="<base>/demo.js" defer></script>
 * data-base is the directory that holds engine.wasm, illuminants.txt and reflectances.txt, with a
 * trailing slash; without it the directory of the page is used.
 * The interface is in Chinese (default), English or Japanese, chosen with the switch in the corner of the
 * frame; data-lang="en" or "ja" on the element selects another starting language. The choice made with
 * the switch is remembered in localStorage.
 */
(() => {
  'use strict';

  const SCALE = 3;               // canvas pixels per scene pixel
  const SPEED = 52;              // walking speed of the dot, scene pixels per second
  const RADIUS = 3;              // collision radius of the dot
  const LAMP_Z = 18;             // height of the lamp: above crates and barrels, below walls and pillars
  const LAMP_INTENSITY = 2000;   // luminous intensity of the lamp (relative)
  const FRAME_MS = 33;           // redraw interval while lights flicker (a swaying candle makes the
                                 // engine recompute the lamp's geometry every frame)
  const MOVES = {                // by KeyboardEvent.code: independent of layout and case
    ArrowUp: [0, -1], KeyW: [0, -1], ArrowDown: [0, 1], KeyS: [0, 1],
    ArrowLeft: [-1, 0], KeyA: [-1, 0], ArrowRight: [1, 0], KeyD: [1, 0],
  };

  // ---------------------------------------------------------------- Languages
  // Interface languages in the order of the switch; the first is the default. T() holds one text in all
  // of them; '{}' marks where a value is inserted.
  const LANGS = ['zh', 'en', 'ja'];
  const LANG_NAMES = { zh: '中文', en: 'English', ja: '日本語' };
  const HTML_LANG = { zh: 'zh-CN', en: 'en', ja: 'ja' };
  const LANG_KEY = 'spectral-pixels-lang';
  const T = (zh, en, ja) => ({ zh, en, ja });
  const UI = {
    scene: T('光谱打光的场景。获得焦点后可用方向键或 WASD 移动小点',
      'Spectrally lit scene. When it has focus, move the dot with the arrow keys or WASD',
      '分光ライティングの場面。フォーカス中は矢印キーか WASD で点を動かせる'),
    focus: T('点一下画面，就能用键盘走动', 'Click the scene to walk with the keyboard', '画面をクリックするとキーボードで歩ける'),
    walk: T('走动', 'Walk', '移動'),
    arrows: T(' 或方向键', ' or arrow keys', ' か矢印キー'),
    hold: T('点按', 'Hold', '長押し'),
    holdText: T('按住画面，小点朝指针走', 'Hold the pointer down to walk towards it', '押したままにすると点がポインタへ歩く'),
    look: T('查看', 'Inspect', '表示'),
    lookText: T('指针移到哪里，就看哪里的光谱', 'Point anywhere to see the spectra there', 'ポインタの位置のスペクトルを表示'),
    language: T('界面语言', 'Interface language', '表示言語'),
    lamp: T('提灯', 'Lamp', 'ランプ'),
    window: T('窗外', 'Window', '窓の外'),
    materials: T('材料', 'Materials', '素材'),
    shutters: T('关上百叶窗', 'Shutters closed', 'よろい戸を閉めた状態'),
    brightness: T('提灯亮度', 'Lamp brightness', 'ランプの明るさ'),
    exposure: T('曝光', 'Exposure', '露出'),
    lightChart: T('光源的相对光谱功率分布', 'Relative spectral power distributions of the lights', '光源の相対分光分布'),
    reflChart: T('材料的光谱反射率', 'Spectral reflectance of the material', '素材の分光反射率'),
    probeChart: T('一点的入射光谱与反射光谱', 'Incident and reflected spectra at one point', '1 点の入射スペクトルと反射スペクトル'),
    lightTitle: T('光源的光谱', 'Light spectra', '光源のスペクトル'),
    reflTitle: T('{}的反射率', 'Reflectance: {}', '{}の反射率'),
    outside: T('窗外', 'Outside', '窓の外'),
    outsideText: T('窗洞处显示窗外景色，不是屋内的表面。', 'The opening shows the view outside, not a surface in the room.',
      '窓には外の景色が見えている。室内の面ではない。'),
    probePointer: T('指针所指的光谱', 'Spectra at the pointer', 'ポインタ位置のスペクトル'),
    probeDot: T('小点脚下的光谱', 'Spectra at the dot', '点の足元のスペクトル'),
    lampOf: T('提灯：{}', 'Lamp: {}', 'ランプ：{}'),
    windowOf: T('窗外：{}', 'Window: {}', '窓の外：{}'),
    perPeak: T('各自按峰值归一', 'Each scaled to its peak', 'それぞれのピークで正規化'),
    incident: T('照到这里的光', 'Light arriving here', 'ここに届く光'),
    reflected: T('反射出来的光（以入射光的峰值归一）', 'Reflected light (scaled to the peak of the incident light)',
      '反射される光（入射光のピークで正規化）'),
    license: T('许可：', ' License: ', 'ライセンス：'),
    end: T('。', '. ', '。'),
    axis: T('横轴为波长（nm）。', 'Horizontal axes: wavelength (nm).', '横軸は波長（nm）。'),
    badLine: T('数据行格式错误：{}', 'malformed data line: {}', 'データ行の形式が正しくない：{}'),
    missing: T('数据中缺少：{}', 'missing from the data: {}', 'データにない：{}'),
    failed: T('演示加载失败：{}', 'The demo could not be loaded: {}', 'デモを読み込めなかった：{}'),
  };
  const fill = (text, value) => text.replace('{}', value);

  // Starting language: the one chosen earlier with the switch, else data-lang, else the default.
  function startLang(root) {
    let saved = null;
    try { saved = localStorage.getItem(LANG_KEY); } catch (e) { /* storage unavailable */ }
    return [saved, root.dataset.lang].find((l) => LANGS.includes(l)) || LANGS[0];
  }

  // ---------------------------------------------------------------- Lights, views and materials

  // Lamps the dot can carry. flicker: relative amplitude of the brightness variation and the frequencies
  // (Hz) it is made of; sway: amplitude of the flame's motion, in scene pixels.
  const LAMPS = [
    { id: 'planck-1900', label: T('蜡烛', 'Candle', 'ろうそく'), icon: 'candle', flicker: { amp: 0.1, hz: [1.1, 2.3, 4.7, 9.1] }, sway: 0.5 },
    { id: 'cie-a', label: T('白炽灯', 'Incan\u00addescent', '白熱電球'), icon: 'bulb', flicker: { amp: 0.05, hz: [0.7, 1.9, 3.7] } },
    { id: 'cie-hp1', label: T('高压钠灯', 'Sodium', 'ナトリウム灯'), icon: 'street', flicker: { amp: 0.06, hz: [0.4, 1.3, 2.9] } },
    { id: 'cie-fl7', label: T('日光色灯管', 'Daylight tube', '昼光色蛍光灯'), icon: 'tube', flicker: { amp: 0.04, hz: [0.8, 2.7, 5.3] } },
    { id: 'cie-fl11', label: T('三基色灯管', 'Tri-band tube', '三波長蛍光灯'), icon: 'tube', flicker: { amp: 0.04, hz: [0.9, 2.9, 6.1] } },
    { id: 'cie-led-b4', label: T('白光 LED', 'White LED', '昼白色 LED'), icon: 'led', flicker: { amp: 0.025, hz: [0.6, 1.3, 3.1] } },
    { id: 'cie-led-b5', label: T('冷白 LED', 'Cool LED', '昼光色 LED'), icon: 'torch', flicker: { amp: 0.025, hz: [0.6, 1.3, 3.1] } },
    { id: 'cie-led-rgb1', label: T('RGB LED', 'RGB LED', 'RGB LED'), icon: 'rgb', flicker: { amp: 0.025, hz: [0.6, 1.3, 3.1] } },
  ];

  // Views outside the window. Their lights use engine lights 1, 2, …: sun is a directional light (dir points
  // towards the sun), window is skylight with the opening as an area light, point is a point light outside.
  // The daytime views lower the base exposure by 1 EV, as a camera's automatic exposure would.
  const WINDOWS = [
    { label: T('关窗', 'Closed', '閉める'), view: 'closed', ev: 0, lights: [] },
    { label: T('晴天阳光', 'Sunlight', '日差し'), view: 'sun', ev: -1, flicker: { amp: 0.08, hz: [0.15, 0.33] }, lights: [
      { kind: 'sun', spd: 'cie-d55', dir: [0.35, -0.85, 0.45], strength: 36 },
      { kind: 'window', spd: 'cie-daylight-10000', strength: 40 }] },
    { label: T('阴天', 'Overcast', '曇り'), view: 'overcast', ev: -1, flicker: { amp: 0.06, hz: [0.12, 0.29] }, lights: [
      { kind: 'window', spd: 'cie-d75', strength: 50 }] },
    { label: T('晴天背阴', 'Clear sky', '晴天の日陰'), view: 'sky', ev: -1, flicker: { amp: 0.05, hz: [0.1, 0.23] }, lights: [
      { kind: 'window', spd: 'cie-daylight-10000', strength: 60 }] },
    { label: T('高压钠路灯', 'Street lamp', '街灯'), view: 'night', ev: 0, flicker: { amp: 0.06, hz: [0.5, 1.4, 3.1] }, lights: [
      { kind: 'point', spd: 'cie-hp1', at: [132, -40, 44], strength: 30000 }] },
  ];

  // Materials. Parts are in the order of PART_* in scene.h; pick is the initial choice; style is a texture
  // style from scene.h.
  const WOODS = [
    { label: T('浅色清漆木', 'Light varnish', 'ニス塗り・明'), main: 'KLUM-L002' },
    { label: T('深色清漆木', 'Dark varnish', 'ニス塗り・暗'), main: 'KLUM-L001' },
    { label: T('绿漆木', 'Green paint', '緑の塗装'), main: 'KLUM-L102' },
    { label: T('蓝漆木', 'Blue paint', '青の塗装'), main: 'KLUM-L105' },
  ];
  const PARTS = [
    { name: T('地板', 'Floor', '床'), pick: 2, options: [
      { label: T('浅色木地板', 'Light wood', '明るい木'), main: 'KLUM-L002', style: 0 },
      { label: T('深色木地板', 'Dark wood', '暗い木'), main: 'KLUM-L001', style: 0 },
      { label: T('红砖', 'Red brick', '赤レンガ'), main: 'KLUM-B002', joint: 'KLUM-C001', style: 1 },
      { label: T('陶土方砖', 'Clay tile', '陶製タイル'), main: 'KLUM-D301', joint: 'KLUM-C001', style: 2 },
      { label: T('白色石灰岩', 'Limestone', '白い石灰岩'), main: 'KLUM-G002', style: 3 },
      { label: T('黄色砂岩', 'Sandstone', '黄色い砂岩'), main: 'KLUM-J106', joint: 'KLUM-C001', style: 4 },
      { label: T('红色花岗岩', 'Red granite', '赤い花崗岩'), main: 'KLUM-F103', style: 5 },
      { label: T('灰色混凝土', 'Grey concrete', 'コンクリート'), main: 'KLUM-E405', style: 6 }] },
    { name: T('墙', 'Walls', '壁'), pick: 0, options: [
      { label: T('粗面石灰岩', 'Limestone', '粗面の石灰岩'), main: 'KLUM-G001', joint: 'KLUM-C001', style: 0 },
      { label: T('灰色砾岩', 'Conglo\u00admerate', '礫岩'), main: 'KLUM-K004', joint: 'KLUM-C001', style: 0 },
      { label: T('红砖', 'Red brick', '赤レンガ'), main: 'KLUM-B002', joint: 'KLUM-C001', style: 1 },
      { label: T('米色砖', 'Beige brick', 'ベージュレンガ'), main: 'KLUM-B103', joint: 'KLUM-C001', style: 1 },
      { label: T('白色灰泥', 'White plaster', '白い塗り壁'), main: 'KLUM-I201', style: 2 },
      { label: T('米色灰泥', 'Beige plaster', '砂色の塗り壁'), main: 'KLUM-I203', style: 2 },
      { label: T('粉红灰泥', 'Pink plaster', 'ピンク塗り壁'), main: 'KLUM-I101', style: 2 },
      { label: T('浅色混凝土', 'Concrete', 'コンクリート'), main: 'KLUM-E301', style: 3 }] },
    { name: T('石柱', 'Pillars', '石柱'), pick: 0, options: [
      { label: T('白色石灰岩', 'Limestone', '白い石灰岩'), main: 'KLUM-G002', style: 0 },
      { label: T('红色砂岩', 'Sandstone', '赤い砂岩'), main: 'KLUM-J003', style: 0 },
      { label: T('灰色砾岩', 'Conglo\u00admerate', '礫岩'), main: 'KLUM-K004', style: 0 },
      { label: T('浅色混凝土', 'Concrete', 'コンクリート'), main: 'KLUM-E301', style: 0 },
      { label: T('红色花岗岩', 'Red granite', '赤い花崗岩'), main: 'KLUM-F103', style: 1 },
      { label: T('灰色花岗岩', 'Grey granite', '灰色の花崗岩'), main: 'KLUM-F004', style: 1 }] },
    { name: T('木箱', 'Crates', '木箱'), pick: 0, options: WOODS.map((w) => ({ ...w, style: 0 })) },
    { name: T('木桶', 'Barrels', '樽'), pick: 0, options: WOODS.map((w) => ({ ...w, style: 0 })) },
    { name: T('窗框', 'Frame', '窓枠'), pick: 1, options: WOODS.map((w) => ({ ...w, style: 0 })) },
  ];

  // ---------------------------------------------------------------- Pixel-art icons (16 × 16)
  // k outline  d dark grey  s silver  w white  r g b the three LED dies  L light  l highlight of the light.
  // The colours of L and l are computed by the engine from the spectrum.
  const ICONS = {
    candle: [
      '................',
      '.......L........',
      '......LlL.......',
      '......LlL.......',
      '.......L........',
      '.......k........',
      '.....kkkkk......',
      '.....kwwwk......',
      '.....kwwwk......',
      '.....kwwwk......',
      '.....kwwwk......',
      '.....kwwwk......',
      '...kkkkkkkkk....',
      '...kdddddddk....',
      '....kkkkkkk.....',
      '................',
    ],
    bulb: [
      '................',
      '......kkkk......',
      '.....kLLLLk.....',
      '....kLllLLLk....',
      '....kLlLLLLk....',
      '....kLLLLLLk....',
      '....kLLLLLLk....',
      '.....kLLLLk.....',
      '......kLLk......',
      '......kssk......',
      '......kddk......',
      '......kssk......',
      '......kddk......',
      '.......kk.......',
      '................',
      '................',
    ],
    street: [
      '................',
      '..kkkkkkk.......',
      '.kdddddddkkkk...',
      '.kLLLLLLLdddk...',
      '..kLllLLkkkdk...',
      '...kkkkk...dk...',
      '...........dk...',
      '...........dk...',
      '...........dk...',
      '...........dk...',
      '...........dk...',
      '...........dk...',
      '...........dk...',
      '..........kddk..',
      '.........kddddk.',
      '................',
    ],
    tube: [
      '................',
      '................',
      '................',
      '..kkkkkkkkkkkk..',
      '..kddddddddddk..',
      '..kkkkkkkkkkkk..',
      '...k........k...',
      '.kkkkkkkkkkkkkk.',
      '.ksLLLLLLLLLLsk.',
      '.kslllllllllLsk.',
      '.ksLLLLLLLLLLsk.',
      '.kkkkkkkkkkkkkk.',
      '................',
      '................',
      '................',
      '................',
    ],
    led: [
      '................',
      '......kkkk......',
      '.....kLLLLk.....',
      '....kLllLLLk....',
      '....kLlLLLLk....',
      '....kLLLLLLk....',
      '....kLLLLLLk....',
      '...kkkkkkkkkk...',
      '...kssssssssk...',
      '...kkkkkkkkkk...',
      '.....ks..sk.....',
      '.....ks..sk.....',
      '.....ks..sk.....',
      '.....ks...sk....',
      '.....kk...kk....',
      '................',
    ],
    torch: [
      '.......LL.......',
      '..L....LL....L..',
      '...L........L...',
      '....kkkkkkkk....',
      '....klllllLk....',
      '....kkkkkkkk....',
      '.....kssssk.....',
      '......kssk......',
      '......kddk......',
      '......kddk......',
      '......kssk......',
      '......kddk......',
      '......kddk......',
      '......kddk......',
      '......kkkk......',
      '................',
    ],
    rgb: [
      '................',
      '................',
      '..kkkkkkkkkkkk..',
      '..kssssssssssk..',
      '..ksddddddddsk..',
      '..ksdrrdddddsk..',
      '..ksdrrdddddsk..',
      '..ksdddggdddsk..',
      '..ksdddggdddsk..',
      '..ksdddddbbdsk..',
      '..ksdddddbbdsk..',
      '..ksddddddddsk..',
      '..kssssssssssk..',
      '..kkkkkkkkkkkk..',
      '................',
      '................',
    ],
  };
  const ICON_COLORS = { k: '#2a2633', d: '#5d6170', s: '#b8bcc6', w: '#f4f1ea', r: '#e0443a', g: '#3fbf5a', b: '#3d6be0' };

  // Palettes of the views: top and bottom of the sky, far hills, near hills, walls, roofs. In daytime the
  // sky is also tinted by the colour of the light.
  const VIEWS = {
    sun: { sky: [[96, 168, 240], [208, 232, 252]], far: '#8dbb8a', near: '#6aa64e', wall: '#f2ebdc', roof: '#c45a3c' },
    sky: { sky: [[70, 140, 232], [208, 232, 252]], far: '#8dbb8a', near: '#6aa64e', wall: '#f2ebdc', roof: '#c45a3c' },
    overcast: { sky: [[150, 156, 166], [218, 222, 228]], far: '#98a39a', near: '#7d8f72', wall: '#cfcfca', roof: '#94695c' },
    night: { sky: [[10, 14, 38], [30, 38, 74]], far: '#0d1020', near: '#080a14', wall: '#141826', roof: '#0b0e18' },
  };

  // ---------------------------------------------------------------- Helpers

  const css = (rgb) => `rgb(${rgb[0]},${rgb[1]},${rgb[2]})`;
  const lighten = (rgb, t) => rgb.map((v) => Math.round(v + (255 - v) * t));
  const peak = (v) => { const m = Math.max(...v) || 1; return Array.from(v, (a) => a / m); };

  function el(tag, attrs = {}, ...children) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (k === 'class') e.className = v;
      else if (k === 'text') e.textContent = v;
      else e.setAttribute(k, v);
    }
    e.append(...children);
    return e;
  }

  function drawIcon(canvas, map, rgb) {
    const g = canvas.getContext('2d'), colors = { ...ICON_COLORS, L: css(rgb), l: css(lighten(rgb, 0.6)) };
    map.forEach((row, y) => [...row].forEach((ch, x) => {
      if (colors[ch]) { g.fillStyle = colors[ch]; g.fillRect(x, y, 1, 1); }
    }));
  }

  // The view outside (pixel art): sky, clouds or stars, hills and houses; the sun on a sunny day, a street
  // lamp at night. light is the colour of the light outside.
  function drawView(canvas, kind, light) {
    const g = canvas.getContext('2d'), w = canvas.width, h = canvas.height, horizon = Math.round(h * 0.6);
    const px = (x, y, c) => { g.fillStyle = c; g.fillRect(x, y, 1, 1); };
    const hash = (x, y) => { const t = Math.sin(x * 127.1 + y * 311.7) * 43758.5453; return t - Math.floor(t); };
    if (kind === 'closed') {                                      // shutters
      for (let y = 0; y < h; y++) { g.fillStyle = y % 2 ? '#4a3524' : '#6b4b33'; g.fillRect(0, y, w, 1); }
      return;
    }
    const p = VIEWS[kind], night = kind === 'night';
    const tint = (c) => css(c.map((v, i) => Math.min(255, Math.round(v * (0.75 + 0.25 * light[i] / 255)))));
    for (let y = 0; y < h; y++)
      for (let x = 0; x < w; x++) {
        const t = Math.min(1, y / horizon), c = p.sky[0].map((v, i) => v + (p.sky[1][i] - v) * t);
        if (night) px(x, y, hash(x, y) > 0.96 && t < 0.8 ? '#d8deff' : css(c.map(Math.round)));
        else if (kind === 'overcast') px(x, y, tint(c.map((v) => v + 14 * Math.sin(x * 0.5 + y * 1.3) * Math.sin(x * 0.17))));
        else px(x, y, tint(c));
      }
    if (kind === 'sun' || kind === 'sky')                         // two clouds
      for (const [fx, fy, r] of [[0.18, 0.28, 3], [0.45, 0.18, 2]]) {
        const cx = Math.round(fx * w), cy = Math.round(fy * h);
        for (let dx = -r; dx <= r; dx++) { px(cx + dx, cy, '#ffffff'); if (Math.abs(dx) < r) px(cx + dx, cy - 1, '#f4f8ff'); }
      }
    if (kind === 'sun') {                                         // the sun: a bright disc and its halo
      const sx = w * 0.76, sy = h * 0.26;
      for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
          const d = Math.hypot(x + 0.5 - sx, y + 0.5 - sy);
          if (d < 1.8) px(x, y, '#fffdf4');
          else if (d < 4.5) px(x, y, `rgba(255,250,225,${(0.7 * (4.5 - d) / 2.7).toFixed(2)})`);
        }
    }
    for (let x = 0; x < w; x++) {                                 // two layers of hills
      const far = horizon + Math.round(1.2 * Math.sin(x * 0.21 + 1)), near = horizon + 1 + Math.round(1.4 * Math.sin(x * 0.33));
      for (let y = far; y < h; y++) px(x, y, p.far);
      for (let y = near; y < h; y++) px(x, y, p.near);
    }
    [0.1, 0.32, 0.6, 0.88].forEach((f, k) => {                    // houses: walls and roofs, lit windows at night
      const hx = Math.round(f * w), hy = horizon + 1;
      for (let dx = 0; dx < 3; dx++) { px(hx + dx, hy, p.wall); px(hx + dx, hy + 1, p.wall); }
      for (let dx = -1; dx < 4; dx++) px(hx + dx, hy - 1, p.roof);
      if (night && k % 2 === 0) px(hx + 1, hy, '#ffd27a');
    });
    if (night) {                                                  // street lamp: pole, head and halo
      const lx = Math.round(0.75 * w), ly = Math.round(h * 0.25);
      for (let y = ly; y < h; y++) px(lx, y, '#262b3d');
      for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
          const e = Math.hypot(x + 0.5 - (lx - 1), y + 0.5 - ly);
          if (e < 5) px(x, y, `rgba(${light.join(',')},${(0.6 * (5 - e) / 5).toFixed(2)})`);
        }
      px(lx - 2, ly, css(light));
      px(lx - 1, ly, css(lighten(light, 0.2)));
      px(lx - 1, ly - 1, '#262b3d');
      px(lx, ly - 1, '#262b3d');
    }
  }

  // Spectral chart (SVG): the grid is drawn once, afterwards only the curves are updated.
  const CHART = { w: 300, h: 140, left: 30, right: 8, top: 8, bottom: 118 };
  const SVG = 'http://www.w3.org/2000/svg';
  function makeChart(nw) {
    const svg = document.createElementNS(SVG, 'svg');
    svg.setAttribute('viewBox', `0 0 ${CHART.w} ${CHART.h}`);
    svg.setAttribute('class', 'spl-chart');
    svg.setAttribute('role', 'img');
    const x = (i) => CHART.left + (CHART.w - CHART.left - CHART.right) * i / (nw - 1);
    const y = (v) => CHART.bottom - (CHART.bottom - CHART.top) * Math.min(v, 1.04);
    const add = (tag, attrs, text) => {
      const e = document.createElementNS(SVG, tag);
      for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
      if (text != null) e.textContent = text;
      svg.append(e);
      return e;
    };
    for (const wl of [400, 500, 600, 700]) {
      const xi = x((wl - 400) / 5);
      add('line', { class: 'spl-grid', x1: xi, x2: xi, y1: CHART.top, y2: CHART.bottom });
      add('text', { class: 'spl-tick', x: xi, y: CHART.bottom + 14, 'text-anchor': 'middle' }, wl);
    }
    for (const t of [0, 0.5, 1]) {
      add('line', { class: 'spl-grid', x1: CHART.left, x2: CHART.w - CHART.right, y1: y(t), y2: y(t) });
      add('text', { class: 'spl-tick', x: CHART.left - 5, y: y(t) + 3.5, 'text-anchor': 'end' }, t);
    }
    const layer = add('g', {});
    const path = (v) => Array.from(v, (a, i) => `${i ? 'L' : 'M'}${x(i).toFixed(1)} ${y(a).toFixed(1)}`).join('');
    return {
      svg,
      set(curves) {                                               // [{ v, cls, fill }], v already scaled to 0–1
        while (layer.childNodes.length > 2 * curves.length) layer.lastChild.remove();
        while (layer.childNodes.length < 2 * curves.length) layer.append(document.createElementNS(SVG, 'path'));
        curves.forEach((c, k) => {
          const area = layer.childNodes[2 * k], line = layer.childNodes[2 * k + 1], d = path(c.v);
          area.setAttribute('d', c.fill ? `${d}L${x(nw - 1)} ${CHART.bottom}L${x(0)} ${CHART.bottom}Z` : '');
          area.setAttribute('class', 'spl-area');
          area.setAttribute('fill', c.fill || 'none');
          line.setAttribute('d', d);
          line.setAttribute('class', `spl-curve ${c.cls || ''}`);
        });
      },
    };
  }

  // ---------------------------------------------------------------- Data

  const LICENSES = {
    'CC-BY-SA-4.0': { name: 'CC BY-SA 4.0', href: 'https://creativecommons.org/licenses/by-sa/4.0/' },
    'GPL-2.0-only': { name: 'GPL-2.0', href: 'GPL-2.0.txt' },
  };

  // Data file: lines starting with # are comments; "#!en", "#!zh" and "#!ja" lines give the attribution
  // in each language and the SPDX line the license. Every other line holds an id, the English, Chinese
  // and Japanese names and nw values, separated by tabs.
  function parseData(text, into, nw, lang) {
    const credit = {};
    let license = null;
    for (const line of text.split('\n')) {
      const m = /^#!(\w+) (.*)$/.exec(line);
      if (m) credit[m[1]] = m[2].trim();
      else if (line.startsWith('# SPDX-License-Identifier:')) license = LICENSES[line.split(':')[1].trim()];
      else if (line && !line.startsWith('#')) {
        const f = line.split('\t');
        if (f.length !== 4 + nw || f.slice(4).some((v) => v === '' || !Number.isFinite(+v))) throw new Error(fill(UI.badLine[lang], f[0]));
        into[f[0]] = { names: { en: f[1], zh: f[2], ja: f[3] }, v: Float32Array.from(f.slice(4), Number) };
      }
    }
    return { credit, license };
  }

  async function load(base, lang) {
    const get = async (name) => {
      const r = await fetch(new URL(name, base));
      if (!r.ok) throw new Error(`${name}: HTTP ${r.status}`);
      return r;
    };
    const [wasm, illuminants, reflectances] = await Promise.all([
      get('engine.wasm').then((r) => r.arrayBuffer()).then((b) => WebAssembly.instantiate(b, {})),
      get('illuminants.txt').then((r) => r.text()),
      get('reflectances.txt').then((r) => r.text()),
    ]);
    const engine = wasm.instance.exports;
    engine._initialize();
    const nw = new Int32Array(engine.memory.buffer, engine.demo_constants(), 5)[0], spectra = {};
    const credits = [parseData(illuminants, spectra, nw, lang), parseData(reflectances, spectra, nw, lang)];
    const needed = [...LAMPS.map((l) => l.id), ...WINDOWS.flatMap((w) => w.lights.map((l) => l.spd)), 'cie-d65',
                    ...PARTS.flatMap((p) => p.options.flatMap((o) => [o.main, o.joint].filter(Boolean)))];
    const missing = [...new Set(needed.filter((id) => !spectra[id]))];
    if (missing.length) throw new Error(fill(UI.missing[lang], missing.join(', ')));
    return { engine, spectra, credits };
  }

  // ---------------------------------------------------------------- Demo

  let instances = 0;                                              // makes element ids unique per demo
  function mount(root, base, lang, { engine: E, spectra, credits }) {
    const uid = `spl${++instances}`;
    const tr = (text) => text[lang];
    const nameOf = (id) => spectra[id].names[lang];
    const relabel = [];                                           // functions that write the texts of the current language
    const bind = (fn) => { relabel.push(fn); fn(); };
    const [NW, W, H, ROWS, SWATCH] = new Int32Array(E.memory.buffer, E.demo_constants(), 5);
    const LIGHTS = 1 + Math.max(...WINDOWS.map((w) => w.lights.length));   // the lamp, then the lights outside

    // Buffers in the module's memory. An allocation may grow the memory, so every typed-array view is created
    // after all allocations, at the time of use.
    const alloc = (bytes) => { const p = E.demo_alloc(bytes); if (!p) throw new Error('out of memory'); return p; };
    const buf = {
      config: alloc(16), spectra: alloc(3 * NW * 4), frame: alloc(W * H * 4), probe: alloc(2 * NW * 4),
      rgb: alloc(4), rect: alloc(16), swatch: alloc(SWATCH * SWATCH * 4), created: alloc(4),
    };
    new Int32Array(E.memory.buffer, buf.config, 4).set([W, H, ROWS, LIGHTS]);
    if (E.spl_create(buf.config, buf.created) !== 0) throw new Error('could not create the engine');
    const engine = new Uint32Array(E.memory.buffer, buf.created, 1)[0], room = E.scene_create(engine);
    if (!room) throw new Error('could not create the room');
    const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
    const phase = Array.from({ length: 8 }, () => Math.random() * 2 * Math.PI);

    const put = (slot, v) => {                                    // copies a spectrum into the engine's scratch area; returns its address
      const at = buf.spectra + slot * NW * 4;
      new Float32Array(E.memory.buffer, at, NW).set(v);
      return at;
    };
    const rgbOf = (id) => { E.spl_light_rgb(put(0, spectra[id].v), buf.rgb); return Array.from(new Uint8Array(E.memory.buffer, buf.rgb, 3)); };

    const state = { lamp: 0, window: 1, intensity: 1, ev: 0, me: { x: 88, y: 104 } };
    const lamps = LAMPS.map((L) => ({ ...L, rgb: rgbOf(L.id), shape: peak(spectra[L.id].v) }));
    const windows = WINDOWS.map((w) => ({
      ...w, lights: w.lights.map((L) => ({ ...L, rgb: rgbOf(L.spd), shape: peak(spectra[L.spd].v) })),
    }));
    const viewColor = (w) => (w.lights.length ? w.lights[w.lights.length - 1].rgb : [255, 255, 255]);

    // ---- Engine settings
    const picks = PARTS.map((P) => P.pick);
    const option = (p) => PARTS[p].options[picks[p]];
    function applyPart(p) {
      const o = option(p);
      E.scene_set_material(room, p, o.style, put(0, spectra[o.main].v), o.joint ? put(1, spectra[o.joint].v) : 0);
    }
    function applyLamp() { E.spl_light_set_spectrum(engine, 0, put(0, spectra[lamps[state.lamp].id].v)); }
    function applyWindow() {
      const w = windows[state.window];
      E.scene_set_window(room, w.lights.length ? 1 : 0);
      E.scene_build(room);
      for (let l = 1; l < LIGHTS; l++) if (!w.lights[l - 1]) E.spl_light_disable(engine, l);
      w.lights.forEach((L, k) => E.spl_light_set_spectrum(engine, k + 1, put(0, spectra[L.spd].v)));
      applyExposure();
    }
    function applyExposure() { E.spl_set_exposure(engine, 2 ** (windows[state.window].ev + state.ev)); }
    function wave(f, t, seed) {                                   // a sum of sines: 1 + amp × a normalized variation
      if (!f || reduceMotion.matches) return 1;
      let s = 0, n = 0;
      f.hz.forEach((hz, j) => { const a = 1 / (j + 1); s += a * Math.sin(2 * Math.PI * hz * t + phase[(j + seed) % 8]); n += a; });
      return 1 + f.amp * s / n;
    }
    function placeLights(t) {                                     // t in seconds: flicker of all lights and sway of the flame
      const lamp = lamps[state.lamp], w = windows[state.window];
      const sway = reduceMotion.matches ? 0 : lamp.sway || 0;
      const dx = sway * Math.sin(2 * Math.PI * 0.9 * t + phase[5]), dy = 0.5 * sway * Math.sin(2 * Math.PI * 1.7 * t + phase[6]);
      E.spl_light_set_point(engine, 0, state.me.x + dx, state.me.y + dy, LAMP_Z, LAMP_INTENSITY * state.intensity * wave(lamp.flicker, t, 0));
      const k = wave(w.flicker, t, 3);
      w.lights.forEach((L, j) => {
        if (L.kind === 'sun') E.spl_light_set_directional(engine, j + 1, ...L.dir, L.strength * k);
        else if (L.kind === 'window') E.scene_window_light(room, j + 1, L.strength * k);
        else E.spl_light_set_point(engine, j + 1, ...L.at, L.strength * k);
      });
    }

    PARTS.forEach((_, p) => applyPart(p));
    applyLamp();
    applyWindow();

    // ---- Frame
    root.replaceChildren();
    const canvas = el('canvas', { class: 'spl-canvas', tabindex: '0', role: 'img' });
    canvas.width = W * SCALE;
    canvas.height = H * SCALE;
    bind(() => canvas.setAttribute('aria-label', tr(UI.scene)));
    const tag = el('div', { class: 'spl-tag', 'aria-hidden': 'true' });
    const focusHint = el('div', { class: 'spl-focus' });
    bind(() => { focusHint.textContent = tr(UI.focus); });
    const langSwitch = el('div', { class: 'spl-lang', role: 'group' });
    const langButtons = LANGS.map((l) => {
      const b = el('button', { type: 'button', lang: HTML_LANG[l], text: LANG_NAMES[l] });
      b.addEventListener('click', () => setLang(l));
      langSwitch.append(b);
      return b;
    });
    bind(() => {
      langSwitch.setAttribute('aria-label', tr(UI.language));
      langButtons.forEach((b, k) => b.setAttribute('aria-pressed', String(LANGS[k] === lang)));
    });
    const stage = el('div', { class: 'spl-stage' }, canvas, tag, focusHint, langSwitch);
    const text = (t) => { const e = el('span'); bind(() => { e.textContent = tr(t); }); return e; };
    const help = el('div', { class: 'spl-help' },
      el('div', { class: 'spl-help-item' }, el('b', {}, text(UI.walk)),
        el('span', {}, ...['W', 'A', 'S', 'D'].map((k) => el('kbd', { text: k })), text(UI.arrows))),
      el('div', { class: 'spl-help-item' }, el('b', {}, text(UI.hold)), text(UI.holdText)),
      el('div', { class: 'spl-help-item' }, el('b', {}, text(UI.look)), text(UI.lookText)));
    root.append(stage, help);

    // ---- Option buttons
    function optionButton(label, title, art, onPick) {             // label: a T() text; title: a function
      const name = el('span', { class: 'spl-opt-name' });
      const b = el('button', { type: 'button', class: 'spl-opt' }, art, name);
      bind(() => { name.textContent = tr(label); b.title = title(); });
      b.addEventListener('click', onPick);
      return b;
    }
    let groupCount = 0;
    function group(title, body, role = 'group') {
      const id = `${uid}-group-${++groupCount}`;
      body.setAttribute('role', role);
      body.setAttribute('aria-labelledby', id);
      const head = el('div', { class: 'spl-pick-title', id });
      bind(() => { head.textContent = tr(title); });
      return el('div', { class: 'spl-pick' }, head, body);
    }
    const press = (buttons, k) => buttons.forEach((b, j) => b.setAttribute('aria-pressed', String(j === k)));

    const lampRow = el('div', { class: 'spl-pick-row spl-lamps' });
    const lampButtons = lamps.map((L, k) => {
      const icon = el('canvas', { class: 'spl-ico', 'aria-hidden': 'true' });
      icon.width = icon.height = 16;
      drawIcon(icon, ICONS[L.icon], L.rgb);
      const b = optionButton(L.label, () => nameOf(L.id), icon, () => { state.lamp = k; applyLamp(); syncLights(); redraw(); });
      b.style.setProperty('--spl-glow', css(L.rgb));
      lampRow.append(b);
      return b;
    });

    const windowRow = el('div', { class: 'spl-pick-row spl-windows' });
    const windowButtons = windows.map((w, k) => {
      const view = el('canvas', { class: 'spl-ico spl-view', 'aria-hidden': 'true' });
      view.width = 24;
      view.height = 12;
      drawView(view, w.view, viewColor(w));
      const title = () => (w.lights.length ? w.lights.map((L) => nameOf(L.spd)).join(' + ') : tr(UI.shutters));
      const b = optionButton(w.label, title, view, () => { state.window = k; applyWindow(); syncLights(); redraw(); });
      windowRow.append(b);
      return b;
    });

    // Materials: pick a part in the tabs, then its material. One set of buttons per part, created once.
    function swatch(p, o) {
      const c = el('canvas', { class: 'spl-ico spl-swatch', 'aria-hidden': 'true' });
      c.width = c.height = SWATCH;
      E.scene_swatch(p, o.style, put(0, spectra['cie-d65'].v), put(1, spectra[o.main].v), o.joint ? put(2, spectra[o.joint].v) : 0, buf.swatch);
      const ctx = c.getContext('2d'), img = ctx.createImageData(SWATCH, SWATCH);
      img.data.set(new Uint8Array(E.memory.buffer, buf.swatch, SWATCH * SWATCH * 4));
      ctx.putImageData(img, 0, 0);
      return c;
    }
    const tabs = el('div', { class: 'spl-tabs' });
    const panels = el('div', { class: 'spl-panels' });
    let activePart = 0;
    const partTabs = PARTS.map((P, p) => {
      const tabSwatch = el('span', { class: 'spl-tab-swatch' }), tabName = el('span'), tabLabel = el('small');
      const tab = el('button', { type: 'button', class: 'spl-tab', role: 'tab', id: `${uid}-tab-${p}`, 'aria-controls': `${uid}-panel-${p}` },
        tabSwatch, el('span', { class: 'spl-tab-text' }, tabName, tabLabel));
      bind(() => { tabName.textContent = tr(P.name); tabLabel.textContent = tr(option(p).label); });
      const panel = el('div', { class: 'spl-pick-row spl-materials', role: 'tabpanel', id: `${uid}-panel-${p}`, 'aria-labelledby': `${uid}-tab-${p}` });
      const sync = () => { tabSwatch.replaceChildren(swatch(p, option(p))); tabLabel.textContent = tr(option(p).label); };
      const buttons = P.options.map((o, j) => {
        const title = () => nameOf(o.main) + (o.joint ? ` + ${nameOf(o.joint)}` : '');
        return optionButton(o.label, title, swatch(p, o), () => {
          const restyle = option(p).style !== o.style;
          picks[p] = j;
          applyPart(p);
          if (restyle) E.scene_build(room);                           // only a new style needs a rebuild; for a new reflectance the engine just recomputes the spectral sums
          press(buttons, j);
          sync();
          shownPart = null;
          redraw();
        });
      });
      panel.append(...buttons);
      press(buttons, picks[p]);
      sync();
      tab.addEventListener('click', () => { activePart = p; syncTabs(); });
      tabs.append(tab);
      panels.append(panel);
      return { tab, panel };
    });
    function syncTabs() {
      partTabs.forEach(({ tab, panel }, p) => {
        tab.setAttribute('aria-selected', String(p === activePart));
        panel.hidden = p !== activePart;
      });
    }
    syncTabs();
    root.append(group(UI.lamp, lampRow), group(UI.window, windowRow), group(UI.materials, el('div', { class: 'spl-materials-box' }, tabs, panels)));
    tabs.setAttribute('role', 'tablist');

    // ---- Sliders
    const sliders = el('div', { class: 'spl-sliders' });
    function slider(label, min, max, step, format, onInput) {
      const input = el('input', { type: 'range', min, max, step, value: 0 });
      const out = el('output', { text: format(0) });
      bind(() => input.setAttribute('aria-label', tr(label)));
      input.addEventListener('input', () => { out.textContent = format(+input.value); onInput(+input.value); redraw(); });
      input.addEventListener('pointerup', () => canvas.focus({ preventScroll: true }));   // return focus after dragging with a pointer
      sliders.append(el('label', { class: 'spl-slider' }, text(label), input, out));
    }
    slider(UI.brightness, -2, 1, 0.25, (v) => `×${+(2 ** v).toFixed(2)}`, (v) => { state.intensity = 2 ** v; });
    slider(UI.exposure, -2, 2, 0.5, (v) => `${v > 0 ? '+' : v < 0 ? '−' : '±'}${Math.abs(v)} EV`, (v) => { state.ev = v; applyExposure(); });
    root.append(sliders);

    // ---- Charts and attribution
    const charts = el('div', { class: 'spl-charts' });
    function figure(label) {
      const chart = makeChart(NW), title = el('div', { class: 'spl-fig-title' }), caption = el('figcaption');
      bind(() => chart.svg.setAttribute('aria-label', tr(label)));
      charts.append(el('figure', { class: 'spl-fig' }, title, chart.svg, caption));
      return { chart, title, caption };
    }
    const lightFig = figure(UI.lightChart), reflFig = figure(UI.reflChart), probeFig = figure(UI.probeChart);
    bind(() => { lightFig.title.textContent = tr(UI.lightTitle); });
    const creditBox = el('p', { class: 'spl-credit' });
    bind(() => {
      creditBox.replaceChildren();
      credits.forEach(({ credit, license }) => {
        creditBox.append(credit[lang] || credit.en || '');
        if (license) creditBox.append(tr(UI.license), el('a', { href: new URL(license.href, base).href, text: license.name }), tr(UI.end));
      });
      creditBox.append(tr(UI.axis));
    });
    root.append(charts, creditBox);

    // ---- Drawing
    const g = canvas.getContext('2d');
    const frame = el('canvas');
    frame.width = W;
    frame.height = H;
    const frameCtx = frame.getContext('2d'), image = frameCtx.createImageData(W, H);
    E.scene_window_rect(buf.rect);
    const rect = Array.from(new Int32Array(E.memory.buffer, buf.rect, 4));   // window opening: x0, y0, x1, y1
    const outside = el('canvas');
    outside.width = rect[2] - rect[0];
    outside.height = rect[3] - rect[1];

    function syncLights() {
      press(lampButtons, state.lamp);
      press(windowButtons, state.window);
      const L = lamps[state.lamp], w = windows[state.window];
      lightFig.chart.set([{ v: L.shape, fill: css(L.rgb), cls: 'spl-lamp' }, ...w.lights.map((l) => ({ v: l.shape, cls: 'spl-win' }))]);
      lightFig.caption.replaceChildren(el('span', { class: 'spl-sw', style: `background:${css(L.rgb)}` }), fill(tr(UI.lampOf), nameOf(L.id)));
      for (const l of w.lights) lightFig.caption.append(el('br'), el('span', { class: 'spl-leg spl-win' }), fill(tr(UI.windowOf), nameOf(l.spd)));
      lightFig.caption.append(el('br'), tr(UI.perPeak));
      tag.replaceChildren(el('span', { class: 'spl-tag-dot', style: `background:${css(L.rgb)}` }), tr(L.label));
      if (w.lights.length) {
        tag.append(el('span', { class: 'spl-sep', text: '·' }), tr(w.label));
        drawView(outside, w.view, viewColor(w));
      }
      shownPart = null;
    }

    let shownPart = null;                                         // part shown in the reflectance chart; null: needs redrawing
    function showPart(p) {
      if (shownPart === p) return;
      shownPart = p;
      if (p < 0) {
        reflFig.title.textContent = tr(UI.outside);
        reflFig.chart.set([]);
        reflFig.caption.textContent = tr(UI.outsideText);
        return;
      }
      const o = option(p), mats = [o.main, ...(o.joint ? [o.joint] : [])].map((id) => ({ v: spectra[id].v, name: nameOf(id) }));
      reflFig.title.textContent = fill(tr(UI.reflTitle), tr(PARTS[p].name));
      reflFig.chart.set(mats.map((m, j) => ({ v: m.v, cls: j ? 'spl-joint' : 'spl-main' })));
      reflFig.caption.replaceChildren();
      mats.forEach((m, j) => {
        if (j) reflFig.caption.append(el('br'));
        reflFig.caption.append(el('span', { class: `spl-leg ${j ? 'spl-joint' : 'spl-main'}` }), m.name);
      });
    }

    function drawDot() {                                          // the dot is drawn on the floor at its feet; when hidden, as a translucent dashed circle
      const x = state.me.x * SCALE, y = state.me.y * SCALE, hidden = E.scene_hidden(room, state.me.x, state.me.y);
      g.save();
      g.globalAlpha = hidden ? 0.5 : 1;
      g.beginPath();
      g.arc(x, y, 2.4 * SCALE, 0, 2 * Math.PI);
      g.fillStyle = css(lighten(lamps[state.lamp].rgb, 0.55));
      g.fill();
      g.lineWidth = 0.7 * SCALE;
      g.strokeStyle = '#1b1820';
      if (hidden) g.setLineDash([SCALE, SCALE]);
      g.stroke();
      g.restore();
    }

    let hover = null;
    function draw(t) {
      placeLights(t);
      E.spl_render(engine, buf.frame);
      image.data.set(new Uint8Array(E.memory.buffer, buf.frame, W * H * 4));
      frameCtx.putImageData(image, 0, 0);
      g.imageSmoothingEnabled = false;
      g.clearRect(0, 0, canvas.width, canvas.height);
      if (windows[state.window].lights.length)                    // the engine leaves the opening transparent; draw the view first
        g.drawImage(outside, rect[0] * SCALE, rect[1] * SCALE, outside.width * SCALE, outside.height * SCALE);
      g.drawImage(frame, 0, 0, W * SCALE, H * SCALE);
      drawDot();

      const p = hover || state.me;                                // pixel under the pointer, or under the dot without a pointer
      const px = Math.max(0, Math.min(W - 1, Math.floor(p.x))), py = Math.max(0, Math.min(H - 1, Math.floor(p.y)));
      if (hover) {
        g.strokeStyle = 'rgba(255,255,255,0.85)';
        g.lineWidth = 1;
        g.strokeRect(px * SCALE - 1.5, py * SCALE - 1.5, SCALE + 3, SCALE + 3);
      }
      showPart(E.scene_part_at(room, px, py));
      E.spl_probe(engine, px, py, buf.probe, buf.probe + NW * 4);
      const s = new Float32Array(E.memory.buffer, buf.probe, 2 * NW);
      const incident = s.subarray(0, NW), reflected = s.subarray(NW), m = Math.max(...incident) || 1;
      const k = 4 * (py * W + px), rgb = [image.data[k], image.data[k + 1], image.data[k + 2]];
      probeFig.title.textContent = tr(hover ? UI.probePointer : UI.probeDot);
      probeFig.chart.set([
        { v: Array.from(reflected, (a) => a / m), fill: css(rgb), cls: 'spl-out' },
        { v: Array.from(incident, (a) => a / m), cls: 'spl-in' },
      ]);
      probeFig.caption.replaceChildren(el('span', { class: 'spl-leg spl-in' }), tr(UI.incident), el('br'),
        el('span', { class: 'spl-sw', style: `background:${css(rgb)}` }), tr(UI.reflected));
    }

    // ---- Input
    const free = (x, y) => [[-1, -1], [1, -1], [-1, 1], [1, 1]].every(([sx, sy]) => !E.scene_blocked(room, x + sx * RADIUS, y + sy * RADIUS));
    const keys = new Set();
    let target = null;
    function step(dt) {
      let dx = 0, dy = 0;
      for (const code of keys) { dx += MOVES[code][0]; dy += MOVES[code][1]; }
      if (target) { dx = target.x - state.me.x; dy = target.y - state.me.y; if (Math.hypot(dx, dy) < 1.5) dx = dy = 0; }
      const n = Math.hypot(dx, dy);
      if (!n || !dt) return false;
      dx *= SPEED * dt / n;
      dy *= SPEED * dt / n;
      if (free(state.me.x + dx, state.me.y)) state.me.x += dx;    // test each axis separately so that the dot slides along walls
      if (free(state.me.x, state.me.y + dy)) state.me.y += dy;
      return true;
    }
    root.addEventListener('keydown', (e) => {
      if (!MOVES[e.code] || e.metaKey || e.ctrlKey || e.altKey || e.target.tagName === 'INPUT') return;
      e.preventDefault();
      keys.add(e.code);
      wake();
    });
    root.addEventListener('keyup', (e) => keys.delete(e.code));
    root.addEventListener('focusout', (e) => { if (!root.contains(e.relatedTarget)) keys.clear(); });
    window.addEventListener('blur', () => keys.clear());
    // After a button is clicked with a pointer, focus returns to the canvas so that walking with the keyboard
    // continues; buttons operated from the keyboard keep focus.
    root.addEventListener('click', (e) => { if (e.detail && e.target.closest('button')) canvas.focus({ preventScroll: true }); });

    const toScene = (e) => {
      const r = canvas.getBoundingClientRect();
      return { x: (e.clientX - r.left) / r.width * W, y: (e.clientY - r.top) / r.height * H };
    };
    canvas.addEventListener('pointerdown', (e) => {
      canvas.focus({ preventScroll: true });
      canvas.setPointerCapture(e.pointerId);
      target = toScene(e);                                        // walk towards the pressed point, taken as a floor point
      hover = null;
      wake();
      e.preventDefault();
    });
    canvas.addEventListener('pointermove', (e) => {
      if (target) target = toScene(e);
      else if (e.pointerType === 'mouse') { hover = toScene(e); redraw(); }
    });
    const release = () => { target = null; };
    canvas.addEventListener('pointerup', release);
    canvas.addEventListener('pointercancel', release);
    canvas.addEventListener('pointerleave', () => { if (hover) { hover = null; redraw(); } });

    // ---- Animation: redraw every frame while walking and about every FRAME_MS ms while lights flicker;
    // pause while the demo is off screen
    let visible = true, running = false, last = 0, lastFrame = -Infinity, pending = true;
    function tick(now) {
      running = false;
      if (!visible) { last = 0; return; }
      const dt = last ? Math.min(0.05, (now - last) / 1000) : 0;
      last = now;
      const moved = step(dt), animate = !reduceMotion.matches, busy = keys.size > 0 || target !== null;
      if (moved || pending || (animate && now - lastFrame > FRAME_MS)) { draw(now / 1000); lastFrame = now; pending = false; }
      if (animate || busy) wake();
      else last = 0;
    }
    function wake() { if (!running) { running = true; requestAnimationFrame(tick); } }
    function redraw() { pending = true; wake(); }
    new IntersectionObserver((entries) => {
      visible = entries[entries.length - 1].isIntersecting;
      if (visible) wake();
    }).observe(stage);
    reduceMotion.addEventListener('change', redraw);

    function setLang(l) {
      if (l === lang) return;
      lang = l;
      try { localStorage.setItem(LANG_KEY, l); } catch (e) { /* storage unavailable */ }
      root.lang = HTML_LANG[l];
      relabel.forEach((fn) => fn());
      syncLights();
      redraw();
    }

    root.lang = HTML_LANG[lang];
    syncLights();
    redraw();
  }

  function boot() {
    for (const root of document.querySelectorAll('.spl-demo')) {
      const base = new URL(root.dataset.base || './', location.href);    // directory of engine.wasm and the data
      const lang = startLang(root);
      load(base, lang).then((data) => mount(root, base, lang, data)).catch((err) => {
        root.textContent = fill(UI.failed[lang], err.message);
      });
    }
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})();

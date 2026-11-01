/*
 * prep.c — spectral data preparation
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * Converts CIE data tables and the KLUM spectral library into the formats used by this project. All
 * spectra are resampled to 400–700 nm in 5 nm steps (LIGHT_NW = 61 samples, as in light.h).
 *
 * Usage: prep <data directory> <header output directory> <data output directory>
 *
 * Outputs
 *   cmf.h             CIE 1931 2° standard observer colour-matching functions, compiled into the engine.
 *                     CC BY-SA 4.0.
 *   srgb_lut.h        8-bit quantization thresholds of the sRGB encoding, compiled into the engine. They are
 *                     computed in double precision from the transfer function of IEC 61966-2-1:1999, so that
 *                     the engine does not depend on any platform's powf.
 *   illuminants.txt   relative spectral power distributions of lights. CC BY-SA 4.0.
 *   reflectances.txt  spectral reflectances of building materials. GPL-2.0.
 *   Format of the two .txt files: lines starting with # are comments. The "#!en", "#!zh" and "#!ja" lines
 *   carry a short attribution in English, Chinese and Japanese, shown by the demo in its interface
 *   language, and the SPDX line gives the license. Every other line holds an identifier, the English,
 *   Chinese and Japanese names and 61 values, separated by tabs.
 *
 * Inputs (relative to the data directory; all retrieved on 2026-10-31)
 *   CIE data tables, International Commission on Illumination (CIE), CC BY-SA 4.0
 *   (https://creativecommons.org/licenses/by-sa/4.0/):
 *     cie/CIE_xyz_1931_2deg.csv     CIE 1931 colour-matching functions, 2 degree observer         doi:10.25039/CIE.DS.xvudnb9b
 *     cie/CIE_std_illum_A_1nm.csv   CIE standard illuminant A - 1 nm                              doi:10.25039/CIE.DS.8jsxjrsn
 *     cie/CIE_std_illum_D65.csv     CIE standard illuminant D65                                   doi:10.25039/CIE.DS.hjfjmt59
 *     cie/CIE_illum_D55.csv         Relative SPD of CIE illuminant D55                            doi:10.25039/CIE.DS.qewfb3kp
 *     cie/CIE_illum_D75.csv         Relative SPD of CIE illuminant D75                            doi:10.25039/CIE.DS.9fvcmrk4
 *     cie/CIE_illum_Dxx_comp.csv    Components of the relative spectral distribution of daylight  doi:10.25039/CIE.DS.w7zunnny
 *     cie/CIE_illum_FLs.csv         Relative SPDs of illuminants representing typical fluorescent lamps  doi:10.25039/CIE.DS.ukaymjdn
 *     cie/CIE_illum_LEDs.csv        Relative SPDs of illuminants representing typical LED lamps   doi:10.25039/CIE.DS.vgssnyfg
 *     cie/CIE_illum_HPs.csv         Relative SPDs of high pressure discharge lamp illuminants     doi:10.25039/CIE.DS.f6rvvnev
 *   KLUM (Karlsruhe Library of Urban Materials), GPL-2.0:
 *     R. Ilehag, A. Schenk, Y. Huang, S. Hinz, "KLUM: An Urban VNIR and SWIR Spectral Library Consisting of
 *     Building Materials", Remote Sensing 11(18):2149, 2019. doi:10.3390/rs11182149
 *     https://github.com/rebeccailehag/KLUM_library, commit ad68196 (2019-09-18)
 *     klum/KLUM_spectra.csv   sha256 892d6cbdf85f60c7f46d0c2645b66eb96e3145385e753736e0c19344bdcccbba
 *     klum/KLUM_quality.csv   sha256 1847cc806496ef013ddf3d1256bcef703c8dcf6a9554777590798032995d447b
 *
 * Methods
 *   Planckian radiator: second radiation constant c2 = 1.4388 × 10⁻² m·K, the value adopted by ITS-90
 *     (H. Preston-Thomas, "The International Temperature Scale of 1990 (ITS-90)", Metrologia 27(1):3–10,
 *     1990, doi:10.1088/0026-1394/27/1/002).
 *   CIE daylight: following the method of CIE 015:2018 (doi:10.25039/TR.015.2018), the chromaticity (xD, yD)
 *     is computed from the correlated colour temperature and the relative SPD is synthesized from S0, S1 and
 *     S2 (CIE 015:2018, Table 6). The temperature is taken on the current scale, without the 1.4388/1.4380
 *     conversion; M1 and M2 are not rounded (the published CIE tables for D55, D65 and D75 round them to
 *     three decimals first).
 *   CIE tables: the samples at multiples of 5 nm are taken without interpolation.
 *   KLUM: the raw data are reflectances in percent, 350–2500 nm in 1 nm steps. Each output sample is the
 *     mean of the five raw samples within λ ± 2 nm, divided by 100, to reduce noise. Only specimens whose
 *     visible range is original data are used (notes_VNIR = 10 in KLUM_quality.csv).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NW 61                                   /* LIGHT_NW in light.h */
#define WL(i) (400 + 5 * (i))                   /* wavelength of sample i, nm */

static const char *data_dir;

static FILE *open_input(const char *rel) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", data_dir, rel);
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); exit(1); }
  return f;
}

static FILE *open_output(const char *dir, const char *name) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "w");
  if (!f) { perror(path); exit(1); }
  return f;
}

/* CIE table: each line is "wavelength,column 1,column 2,…". Reads column col at 400–700 nm in 5 nm steps. */
static void read_cie(const char *rel, int col, float out[NW]) {
  FILE *f = open_input(rel);
  char line[4096];
  int got = 0;
  while (fgets(line, sizeof line, f)) {
    char *p = line, *end;
    double wl = strtod(p, &end);
    if (end == p) continue;                     /* not a data line */
    p = end;
    for (int c = 0; c < col && p; c++) { p = strchr(p, ','); if (p) p++; }
    if (!p) continue;
    int w = (int)lround(wl);
    if (fabs(wl - w) > 1e-6 || w < 400 || w > 700 || (w - 400) % 5) continue;
    double v = strtod(p, &end);
    if (end == p) { fprintf(stderr, "%s: column %d at %d nm is not a number\n", rel, col, w); exit(1); }
    out[(w - 400) / 5] = (float)v;
    got++;
  }
  fclose(f);
  if (got != NW) { fprintf(stderr, "%s: column %d has %d samples, expected %d\n", rel, col, got, NW); exit(1); }
}

/* Relative SPD of a Planckian radiator, normalized to 100 at 560 nm. */
static void planck(double T, float out[NW]) {
  const double c2 = 1.4388e-2;
  double m[NW];
  for (int i = 0; i < NW; i++) {
    double lambda = WL(i) * 1e-9;
    m[i] = 1 / (pow(lambda, 5) * (exp(c2 / (lambda * T)) - 1));
  }
  for (int i = 0; i < NW; i++) out[i] = (float)(100 * m[i] / m[(560 - 400) / 5]);
}

/* Relative SPD of CIE daylight with correlated colour temperature T (4000–25000 K). */
static void cie_daylight(double T, float out[NW]) {
  float s0[NW], s1[NW], s2[NW];
  read_cie("cie/CIE_illum_Dxx_comp.csv", 1, s0);
  read_cie("cie/CIE_illum_Dxx_comp.csv", 2, s1);
  read_cie("cie/CIE_illum_Dxx_comp.csv", 3, s2);
  double xd = T <= 7000 ? -4.6070e9 / (T * T * T) + 2.9678e6 / (T * T) + 0.09911e3 / T + 0.244063
                        : -2.0064e9 / (T * T * T) + 1.9018e6 / (T * T) + 0.24748e3 / T + 0.237040;
  double yd = -3.000 * xd * xd + 2.870 * xd - 0.275;
  double m = 0.0241 + 0.2562 * xd - 0.7341 * yd;
  double m1 = (-1.3515 - 1.7703 * xd + 5.9114 * yd) / m;
  double m2 = (0.0300 - 31.4424 * xd + 30.0717 * yd) / m;
  for (int i = 0; i < NW; i++) out[i] = (float)(s0[i] + m1 * s1[i] + m2 * s2[i]);
}

#define KLUM_FIRST 350                          /* wavelength of the first sample, nm */
#define KLUM_N 2151                             /* 350–2500 nm in 1 nm steps */

/* Each line of the KLUM files starts with the quoted specimen identifier. Lines may be longer than the
 * buffer, in which case fgets returns them in pieces; only the first piece of a line starts with an
 * identifier, so there are no false matches. */
static int klum_find(FILE *f, const char *key, char *buf, size_t size) {
  while (fgets(buf, (int)size, f))
    if (strncmp(buf, key, strlen(key)) == 0) return 1;
  return 0;
}

static void klum(const char *id, float out[NW]) {
  static char buf[256 * 1024];
  char key[16];
  snprintf(key, sizeof key, "\"%s\",", id);

  FILE *q = open_input("klum/KLUM_quality.csv");
  int vnir = klum_find(q, key, buf, sizeof buf) ? atoi(buf + strlen(key)) : -1;
  fclose(q);
  if (vnir != 10) { fprintf(stderr, "KLUM %s: visible range is not original data (notes_VNIR = %d)\n", id, vnir); exit(1); }

  FILE *f = open_input("klum/KLUM_spectra.csv");
  if (!klum_find(f, key, buf, sizeof buf)) { fprintf(stderr, "KLUM has no sample %s\n", id); exit(1); }
  fclose(f);
  static double r[KLUM_N];
  char *p = buf + strlen(key) - 1;              /* the comma after the identifier */
  for (int k = 0; k < KLUM_N; k++) {
    char *end;
    if (*p != ',') { fprintf(stderr, "KLUM %s: missing comma before value %d\n", id, k + 1); exit(1); }
    r[k] = strtod(p + 1, &end);
    if (end == p + 1) { fprintf(stderr, "KLUM %s: value %d is not a number\n", id, k + 1); exit(1); }
    p = end;
  }
  for (int i = 0; i < NW; i++) {
    int c = WL(i) - KLUM_FIRST;                 /* c − 2 ≥ 48 and c + 2 ≤ 352: within bounds */
    out[i] = (float)((r[c - 2] + r[c - 1] + r[c] + r[c + 1] + r[c + 2]) / 5 / 100);
  }
}

/* One data line. %.9g guarantees that a float survives the round trip through decimal. */
static void emit(FILE *f, const char *id, const char *const name[3], const float v[NW]) {
  fprintf(f, "%s\t%s\t%s\t%s", id, name[0], name[1], name[2]);
  for (int i = 0; i < NW; i++) fprintf(f, "\t%.9g", v[i]);
  fputc('\n', f);
}

static void write_cmf(const char *dir) {
  float cmf[3][NW];
  for (int c = 0; c < 3; c++) read_cie("cie/CIE_xyz_1931_2deg.csv", c + 1, cmf[c]);
  FILE *h = open_output(dir, "cmf.h");
  fprintf(h, "/*\n"
             " * cmf.h — generated by tools/prep.c; do not edit.\n"
             " *\n"
             " * CIE 1931 2° standard observer colour-matching functions x̄(λ), ȳ(λ), z̄(λ), 400–700 nm, 5 nm steps.\n"
             " * Source: CIE, \"CIE 1931 colour-matching functions, 2 degree observer\", doi:10.25039/CIE.DS.xvudnb9b\n"
             " * Processing: samples at multiples of 5 nm, no interpolation.\n"
             " *\n"
             " * License: CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/\n"
             " * SPDX-License-Identifier: CC-BY-SA-4.0\n"
             " */\n");
  fprintf(h, "#if LIGHT_NW != %d\n#error \"cmf.h and LIGHT_NW disagree on the number of samples\"\n#endif\n\n", NW);
  fprintf(h, "static const float CMF[3][LIGHT_NW] = {\n");
  for (int c = 0; c < 3; c++) {
    fprintf(h, "  {");
    for (int i = 0; i < NW; i++) fprintf(h, "%s%.9g", i ? ", " : "", cmf[c][i]);
    fprintf(h, "},\n");
  }
  fprintf(h, "};\n");
  fclose(h);
}

/* 8-bit quantization of the sRGB encoding f: the output code is the largest b with L >= SRGB_LUT[b - 1],
 * which equals round(255 f(L)). The thresholds are the inverse of f at (b − 0.5)/255, computed in double
 * precision and rounded to float. */
static void write_srgb_lut(const char *dir) {
  FILE *h = open_output(dir, "srgb_lut.h");
  fprintf(h, "/*\n"
             " * srgb_lut.h — generated by tools/prep.c; do not edit.\n"
             " *\n"
             " * 8-bit quantization thresholds of the sRGB encoding (IEC 61966-2-1:1999): a linear value L is\n"
             " * encoded as the largest b with L >= SRGB_LUT[b - 1] (0 if there is none), i.e. round(255 * f(L)).\n"
             " *\n"
             " * SPDX-License-Identifier: MIT\n"
             " */\n\n");
  fprintf(h, "static const float SRGB_LUT[255] = {\n");
  for (int b = 1; b <= 255; b++) {
    double v = (b - 0.5) / 255, L = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    fprintf(h, "%s%.9g,%s", (b - 1) % 6 ? " " : "  ", (float)L, b % 6 && b < 255 ? "" : "\n");
  }
  fprintf(h, "};\n");
  fclose(h);
}

static void write_illuminants(const char *dir) {
  FILE *f = open_output(dir, "illuminants.txt");
  fprintf(f, "# illuminants.txt — generated by tools/prep.c; do not edit.\n"
             "# Relative spectral power distributions, 400–700 nm in 5 nm steps.\n"
             "# Each line: identifier, English, Chinese and Japanese names, %d values; separated by tabs.\n"
             "# Sources: CIE data tables, International Commission on Illumination (CIE); see tools/prep.c\n"
             "#   for the DOI of each table.\n"
             "#   CIE daylight is synthesized from S0, S1 and S2 following CIE 015:2018 (doi:10.25039/TR.015.2018);\n"
             "#   the Planckian radiator uses c2 = 1.4388e-2 m·K.\n"
             "# Processing: samples at multiples of 5 nm, no interpolation.\n"
             "# License: CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/\n"
             "# SPDX-License-Identifier: CC-BY-SA-4.0\n"
             "#!en Light spectra: CIE data tables; CIE daylight synthesized following CIE 015:2018.\n"
             "#!zh 光源光谱：CIE 数据表；CIE 日光按 CIE 015:2018 的方法合成。\n"
             "#!ja 光源スペクトル：CIE データ表。CIE 昼光は CIE 015:2018 の方法で合成。\n", NW);
  /* Names in English, Chinese and Japanese */
  static const char *const name[][3] = {
    { "Planckian radiator, 1900 K", "普朗克辐射体，1900 K", "プランク放射体、1900 K" },
    { "CIE standard illuminant A", "CIE 标准照明体 A", "CIE 標準イルミナント A" },
    { "CIE HP1 (standard high-pressure sodium lamp)", "CIE HP1（标准高压钠灯）", "CIE HP1（標準高圧ナトリウムランプ）" },
    { "CIE FL7 (broadband fluorescent lamp)", "CIE FL7（宽带荧光灯）", "CIE FL7（広帯域形蛍光ランプ）" },
    { "CIE FL11 (three-band fluorescent lamp)", "CIE FL11（三基色荧光灯）", "CIE FL11（三波長形蛍光ランプ）" },
    { "CIE LED-B4 (phosphor-converted white LED)", "CIE LED-B4（荧光粉转换型白光 LED）", "CIE LED-B4（蛍光体変換形白色 LED）" },
    { "CIE LED-B5 (phosphor-converted white LED)", "CIE LED-B5（荧光粉转换型白光 LED）", "CIE LED-B5（蛍光体変換形白色 LED）" },
    { "CIE LED-RGB1 (RGB LED)", "CIE LED-RGB1（RGB 三色 LED）", "CIE LED-RGB1（RGB 3 色 LED）" },
    { "CIE standard illuminant D65", "CIE 标准照明体 D65", "CIE 標準イルミナント D65" },
    { "CIE illuminant D55", "CIE 照明体 D55", "CIE イルミナント D55" },
    { "CIE illuminant D75", "CIE 照明体 D75", "CIE イルミナント D75" },
    { "CIE daylight, CCT 10000 K", "CIE 日光，相关色温 10000 K", "CIE 昼光、相関色温度 10000 K" },
  };
  float e[NW];
  planck(1900, e);
  emit(f, "planck-1900", name[0], e);
  read_cie("cie/CIE_std_illum_A_1nm.csv", 1, e);
  emit(f, "cie-a", name[1], e);
  read_cie("cie/CIE_illum_HPs.csv", 1, e);
  emit(f, "cie-hp1", name[2], e);
  read_cie("cie/CIE_illum_FLs.csv", 7, e);
  emit(f, "cie-fl7", name[3], e);
  read_cie("cie/CIE_illum_FLs.csv", 11, e);
  emit(f, "cie-fl11", name[4], e);
  read_cie("cie/CIE_illum_LEDs.csv", 4, e);
  emit(f, "cie-led-b4", name[5], e);
  read_cie("cie/CIE_illum_LEDs.csv", 5, e);
  emit(f, "cie-led-b5", name[6], e);
  read_cie("cie/CIE_illum_LEDs.csv", 7, e);
  emit(f, "cie-led-rgb1", name[7], e);
  read_cie("cie/CIE_std_illum_D65.csv", 1, e);
  emit(f, "cie-d65", name[8], e);
  read_cie("cie/CIE_illum_D55.csv", 1, e);
  emit(f, "cie-d55", name[9], e);
  read_cie("cie/CIE_illum_D75.csv", 1, e);
  emit(f, "cie-d75", name[10], e);
  cie_daylight(10000, e);
  emit(f, "cie-daylight-10000", name[11], e);
  fclose(f);
}

static void write_reflectances(const char *dir) {
  /* KLUM identifier and names. English names are built from the subclass, colour and status fields of
   * KLUM_metadata.csv (a colour or status already named by the subclass is not repeated, and the surface
   * is added where it tells two specimens apart); the Chinese and Japanese names are translations of them. */
  static const char *const sample[][4] = {
    { "L002", "Varnished wood, brown, new", "清漆木材，棕色，新", "ニス塗り木材、茶色、新品" },
    { "L001", "Varnished wood, brown, weathered", "清漆木材，棕色，老化", "ニス塗り木材、茶色、経年" },
    { "L102", "Painted wood, green, weathered", "涂漆木材，绿色，老化", "塗装木材、緑色、経年" },
    { "L105", "Painted wood, blue, weathered", "涂漆木材，蓝色，老化", "塗装木材、青色、経年" },
    { "B002", "Red brick, new", "红砖，新", "赤レンガ、新品" },
    { "B103", "Beige brick, weathered", "米色砖，老化", "ベージュのレンガ、経年" },
    { "C001", "Grey mortar, new", "灰色砂浆，新", "灰色のモルタル、新品" },
    { "D301", "Red matte ceramic, new", "红色哑光陶瓦，新", "赤色つや消し陶器、新品" },
    { "G002", "White limestone, smooth, weathered", "白色石灰岩，光面，老化", "白色石灰岩、平滑、経年" },
    { "G001", "White limestone, uneven, weathered", "白色石灰岩，粗面，老化", "白色石灰岩、粗面、経年" },
    { "J106", "Weathered sandstone, yellow", "风化砂岩，黄色", "風化した砂岩、黄色" },
    { "J003", "Red sandstone, weathered", "红色砂岩，老化", "赤色砂岩、経年" },
    { "F103", "Granite (muscovite), red, weathered", "白云母花岗岩，红色，老化", "花崗岩（白雲母）、赤色、経年" },
    { "F004", "Granite (biotite), grey, weathered", "黑云母花岗岩，灰色，老化", "花崗岩（黒雲母）、灰色、経年" },
    { "E405", "Grey concrete, new", "灰色混凝土，新", "灰色コンクリート、新品" },
    { "E301", "Bright concrete, grey, weathered", "浅色混凝土，灰色，老化", "明色コンクリート、灰色、経年" },
    { "K004", "Conglomerate, grey, weathered", "砾岩，灰色，老化", "礫岩、灰色、経年" },
    { "I201", "Bright plaster, white, new", "浅色灰泥，白色，新", "明色プラスター、白色、新品" },
    { "I203", "Bright plaster, beige, new", "浅色灰泥，米色，新", "明色プラスター、ベージュ、新品" },
    { "I101", "Colorful plaster, pink, weathered", "彩色灰泥，粉红色，老化", "カラープラスター、ピンク、経年" },
  };
  FILE *f = open_output(dir, "reflectances.txt");
  fprintf(f, "# reflectances.txt — generated by tools/prep.c; do not edit.\n"
             "# Spectral reflectances (0–1) of building materials, 400–700 nm in 5 nm steps.\n"
             "# Each line: identifier, English, Chinese and Japanese names, %d values; separated by tabs.\n"
             "# Source: KLUM (Karlsruhe Library of Urban Materials). R. Ilehag, A. Schenk, Y. Huang, S. Hinz,\n"
             "#   \"KLUM: An Urban VNIR and SWIR Spectral Library Consisting of Building Materials\",\n"
             "#   Remote Sensing 11(18):2149, 2019. doi:10.3390/rs11182149\n"
             "#   Data: https://github.com/rebeccailehag/KLUM_library, commit ad68196\n"
             "# Processing: the raw data are reflectances in percent at 1 nm steps; each sample is the mean of\n"
             "#   the five raw samples within λ ± 2 nm, divided by 100.\n"
             "# License: GPL-2.0; the full text is in GPL-2.0.txt in this directory.\n"
             "# SPDX-License-Identifier: GPL-2.0-only\n"
             "#!en Reflectances: KLUM spectral library of urban materials (Ilehag et al., 2019, doi:10.3390/rs11182149).\n"
             "#!zh 反射率：KLUM 城市建材光谱库（Ilehag 等，2019，doi:10.3390/rs11182149）。\n"
             "#!ja 反射率：KLUM 都市建材分光ライブラリ（Ilehag ほか、2019、doi:10.3390/rs11182149）。\n", NW);
  float r[NW];
  for (size_t i = 0; i < sizeof sample / sizeof sample[0]; i++) {
    char id[16], names[3][128];
    klum(sample[i][0], r);
    snprintf(id, sizeof id, "KLUM-%s", sample[i][0]);
    snprintf(names[0], sizeof names[0], "%s (KLUM %s)", sample[i][1], sample[i][0]);
    snprintf(names[1], sizeof names[1], "%s（KLUM %s）", sample[i][2], sample[i][0]);
    snprintf(names[2], sizeof names[2], "%s（KLUM %s）", sample[i][3], sample[i][0]);
    emit(f, id, (const char *const[3]){ names[0], names[1], names[2] }, r);
  }
  fclose(f);
}

int main(int argc, char **argv) {
  if (argc != 4) { fprintf(stderr, "usage: %s <data directory> <header output directory> <data output directory>\n", argv[0]); return 1; }
  data_dir = argv[1];
  write_cmf(argv[2]);
  write_srgb_lut(argv[2]);
  write_illuminants(argv[3]);
  write_reflectances(argv[3]);
  return 0;
}

# spectral-pixels

中文 | [English](README.en.md) | [日本語](README.ja.md)

[![build](https://github.com/FinnChou/spectral-pixels/actions/workflows/build.yml/badge.svg)](https://github.com/FinnChou/spectral-pixels/actions/workflows/build.yml)

逐波长计算的 2D 像素打光。光源用实测或标准化的相对光谱功率分布描述，材料用实测的建材光谱反射率描述；
每个像素在 400–700 nm 上逐波长相乘、积分得到 CIE XYZ，再转换为 sRGB 显示。

引擎是一个以 C11 编写的小型库，可以直接用在自己的程序里。同一份源码既编译为本机程序，也编译为 WebAssembly
（约 27 KB，无导入）在浏览器中运行，两者的输出逐字节一致。

![演示：提着烛光在屋里走动，窗外是晴天](media/demo.webp)

- 在线试玩：<https://finnchou.github.io/spectral-pixels/>
- 介绍文章：[光谱渲染：给一间像素小屋打光](https://finnchou.github.io/posts/Spectral-Pixel-Lighting/)（作者的博客）

## 演示里能做什么

- 用 WASD 或方向键（也可以按住画面）带着提灯在屋里走动，墙、石柱、木箱、木桶都会挡光。
- 提灯可选 8 种光源：普朗克辐射体（1900 K，当作烛光）、CIE 标准照明体 A、高压钠灯 HP1、荧光灯 FL7 与 FL11、
  白光 LED B4 与 B5、RGB LED。
- 窗外可选晴天阳光、阴天、晴天背阴、高压钠路灯，或者关窗。窗洞作为面光源，在地上投出窗格的影子。
- 地板、墙、石柱、木箱、木桶、窗框的材料都可以换，共 20 种 KLUM 实测建材。
- 指针移到哪里，就显示那里材料的反射率，以及照到那一点的光谱和反射出来的光谱。
- 界面有中文、英文、日文三种，在画面右上角切换。

## 在自己的程序里使用

引擎是一个独立的 C11 库：公开接口在 [`include/spl.h`](include/spl.h)，实现是 `src/spl.c` 一个文件（外加生成的
`src/cmf.h`、`src/srgb_lut.h`），只依赖标准库与 libm。把这四个文件放进自己的项目即可，也可以用 `make lib`
编出静态库 `build/libspl.a`；编成动态库时，编译库与使用库的两侧都要定义 `SPL_SHARED`。库没有全局状态，
可以同时创建多个引擎。

引擎只认光谱：每个光源的相对光谱功率分布，以及一张按行编号的反射率表。它不知道材料叫什么，也不知道场景长什么样；
调用方通过几个缓冲区告诉它每个像素看到的是哪个表面、空间里哪些地方挡光。

| 部分 | 接口 |
|---|---|
| 引擎 | `spl_create()` 按 `spl_config`（宽、高、反射率行数、光源数，上限见 `SPL_MAX_*`）创建，`spl_destroy()` 释放；`spl_version()` |
| 场景 | `spl_surfaces()`：每个像素可见表面的位置、法线、反射率行号、纹理明暗<br>`spl_heights()`、`spl_gaps()`、`spl_set_ceiling()`：遮挡（高度场，可开洞）；改动后调用 `spl_invalidate()` |
| 光源 | `spl_light_set_spectrum()`；`spl_light_set_point()` 点光源、`spl_light_set_directional()` 平行光、`spl_light_set_area()` 平行四边形面光源；`spl_light_disable()` |
| 材料与输出 | `spl_set_reflectance()`、`spl_set_ambient()`、`spl_set_exposure()`；`spl_render()` 把 RGBA 写进调用方的缓冲区，`spl_probe()` 给出一点的入射与反射光谱 |
| 颜色工具 | `spl_light_rgb()` 光源本身的颜色，`spl_surface_rgb()` 某照度下表面的显示颜色；不需要引擎 |

可能失败的函数返回 `spl_status`：参数无效（空指针、下标越界、非有限值或超出文档所述范围）时返回 `SPL_ERR_ARGUMENT`，
引擎保持原状；内存不足时 `spl_create()` 返回 `SPL_ERR_MEMORY`。坐标、单位、光谱归一化、几何缓存等约定写在 `spl.h` 的文件头。

最小的例子在 [`examples/minimal.c`](examples/minimal.c)：一盏等能光源照在 50% 灰的地面上，地上有一个 16 px 高的方块。
核心部分如下（省略了错误处理与写文件）：

```c
spl_config config = { 256, 160, 1, 1 }; /* 256 × 160 像素，一行反射率，一个光源 */
spl_engine *engine;
spl_create(&config, &engine);

float light[SPL_NW];
float grey[SPL_NW];
for (int k = 0; k < SPL_NW; k++) {
    light[k] = 1;   /* 等能光源 */
    grey[k] = 0.5f; /* 各波长反射率 50% */
}

/* 俯视：每个像素看到的是地面或方块的顶面 */
spl_surface *surfaces = spl_surfaces(engine);
float *heights = spl_heights(engine);
for (int y = 0; y < 160; y++) {
    for (int x = 0; x < 256; x++) {
        int i = y * 256 + x;
        int block = x >= 120 && x < 136 && y >= 72 && y < 88;
        heights[i] = block ? 16 : 0;
        spl_surface s = { { x + 0.5f, y + 0.5f, heights[i] }, { 0, 0, 1 }, 0, 1 };
        surfaces[i] = s;
    }
}
spl_invalidate(engine);

spl_set_reflectance(engine, 0, grey);
spl_light_set_spectrum(engine, 0, light);
spl_light_set_point(engine, 0, 96, 80, 24, 2000);

static uint8_t rgba[256 * 160 * 4];
spl_render(engine, rgba);
spl_destroy(engine);
```

`make example` 编译并运行它，输出 `build/minimal.ppm`；也可以直接
`cc -std=c11 -Iinclude -Isrc examples/minimal.c src/spl.c -lm`。演示用的场景（斜投影、像素纹理、窗）在
[`demo/`](demo/)，它只通过 `spl.h` 调用引擎，可以当作一个完整一些的例子来读。

版本号见 `SPL_VERSION_MAJOR`、`SPL_VERSION_MINOR`、`SPL_VERSION_PATCH` 与 `spl_version()`。0.x 期间接口仍可能变动，
每次变动都记在 [`CHANGELOG.md`](CHANGELOG.md)。

## 文件

| 路径 | 内容 |
|---|---|
| `include/spl.h` | 库的公开接口 |
| `src/spl.c` | 库的实现：光谱积分、照度、遮挡、环境光估计、色调映射、sRGB 编码 |
| `src/cmf.h`、`src/srgb_lut.h` | 由 `prep` 生成的头文件：CIE 1931 配色函数，sRGB 量化阈值 |
| `demo/scene.h`、`demo/scene.c` | 演示场景：地图、程序生成的像素纹理、斜投影、窗；填写引擎的输入缓冲区 |
| `demo/wasm.c` | WebAssembly 版额外导出的内存分配函数与常量 |
| `tests/test_spl.c` | 单元测试：参数检查、平方反比与余弦定律、遮挡、面光源、环境光、几何缓存、极端输入、多个引擎互不影响等 |
| `tests/reference.txt` | 基准输出的 SHA-256，由 `make check` 核对 |
| `examples/minimal.c` | 最小的完整例子 |
| `tools/prep.c` | 数据预处理：由 CIE 数据表与 KLUM 光谱库生成上面两个头文件与两个数据文件 |
| `tools/render.c` | 命令行渲染程序：出图，并作为比对的基准 |
| `tools/check.mjs` | 本机版与 WebAssembly 版的逐字节比对 |
| `tools/data.sha256` | 原始数据的 SHA-256 |
| `docs/` | 网页演示（GitHub Pages）：`index.html`、`demo.js`、`demo.css`；生成的 `illuminants.txt`、`reflectances.txt`；从 KLUM 复制的许可证全文 `GPL-2.0.txt`。`make` 把 `engine.wasm` 编译到这里 |
| `.github/workflows/build.yml` | CI：用 zig cc、gcc、clang 分别跑单元测试，在 sanitizer 下再跑一遍，编译库与例子，运行 `make check`；推送到 main 时把 `docs/` 连同编译出的 `engine.wasm` 发布到 GitHub Pages |
| `.clang-format` | 代码格式 |
| `CHANGELOG.md` | 版本变动记录 |
| `media/demo.webp` | 本页的动图 |

生成的头文件与数据文件随源码提交，不必下载原始数据就能构建。`engine.wasm` 是编译产物，不提交：本地用 `make`
编译，在线演示由 CI 编译后发布。

## 构建

默认使用 [zig](https://ziglang.org/) 0.17.0 的 `zig cc`。库、单元测试与本机工具用任何 C11 编译器都可以
（例如 `make -B test CC=gcc`）；WebAssembly 版需要 zig；`make check` 另需 Node.js 18 或更新版本。

```sh
make          # docs/engine.wasm 与本机程序 render
make lib      # 静态库 build/libspl.a
make test     # 单元测试
make sanitize # 在 AddressSanitizer 与 UndefinedBehaviorSanitizer 下跑单元测试（clang），并检查头文件能按 C++ 编译
make example  # examples/minimal.c，输出 build/minimal.ppm
make check    # 本机版与 WebAssembly 版逐字节比对，并与 tests/reference.txt 的基准输出核对
make data     # 由原始数据重新生成头文件与数据文件（见下节）
make format   # 用 clang-format 整理 C 源码
make clean
```

本机与 WebAssembly 的结果一致依赖两点：两个目标都以 `-ffp-contract=off` 编译，不把乘法与加法合并为一次舍入；
引擎与场景只使用 IEEE 754 有精确定义的运算（float 与 double 的四则运算、`sqrtf`、`floorf`、`fmodf`、`fabsf`、
`fminf`、`fmaxf`，以及整数与浮点数之间的转换），sRGB 编码查 `prep` 以双精度预先算好的阈值表，不调用 `powf`。
在自己的项目里编译时，如果需要跨平台逐字节一致，也要加 `-ffp-contract=off`；不加并不影响正确性，只是在有 FMA
指令的平台上，个别像素的某个通道可能差 1 级。

`render` 的用法见 `tools/render.c` 的文件头，例如：

```sh
./render docs/illuminants.txt docs/reflectances.txt out.ppm \
  floor=KLUM-B002,KLUM-C001,1 wall=KLUM-G001,KLUM-C001,0 pillar=KLUM-G002,-,0 \
  crate=KLUM-L002,-,0 barrel=KLUM-L002,-,0 frame=KLUM-L001,-,0 \
  light0=point,planck-1900,88,104,18,2000 light1=window,cie-d75,50 exposure=0.5 scale=3 dot=88,104
```

## 原始数据

`make data` 读取 `DATA` 目录（默认 `data/`，不纳入版本管理）下的文件，均于 2026-10-31 获取：

| 文件 | 来源 | DOI / 版本 |
|---|---|---|
| `cie/CIE_xyz_1931_2deg.csv` | CIE 1931 colour-matching functions, 2 degree observer | 10.25039/CIE.DS.xvudnb9b |
| `cie/CIE_std_illum_A_1nm.csv` | CIE standard illuminant A - 1 nm | 10.25039/CIE.DS.8jsxjrsn |
| `cie/CIE_std_illum_D65.csv` | CIE standard illuminant D65 | 10.25039/CIE.DS.hjfjmt59 |
| `cie/CIE_illum_D55.csv` | Relative spectral power distributions of CIE illuminant D55 | 10.25039/CIE.DS.qewfb3kp |
| `cie/CIE_illum_D75.csv` | Relative spectral power distributions of CIE illuminant D75 | 10.25039/CIE.DS.9fvcmrk4 |
| `cie/CIE_illum_Dxx_comp.csv` | Components of the relative spectral distribution of daylight | 10.25039/CIE.DS.w7zunnny |
| `cie/CIE_illum_FLs.csv` | Relative SPDs of illuminants representing typical fluorescent lamps | 10.25039/CIE.DS.ukaymjdn |
| `cie/CIE_illum_LEDs.csv` | Relative SPDs of illuminants representing typical LED lamps | 10.25039/CIE.DS.vgssnyfg |
| `cie/CIE_illum_HPs.csv` | Relative SPDs of high pressure discharge lamp illuminants | 10.25039/CIE.DS.f6rvvnev |
| `klum/KLUM_spectra.csv`、`klum/KLUM_quality.csv`、`klum/LICENSE.txt` | KLUM 城市建材光谱库 | 提交 ad68196 |

下载并核对：

```sh
mkdir -p data/cie data/klum
for f in CIE_xyz_1931_2deg CIE_std_illum_A_1nm CIE_std_illum_D65 CIE_illum_D55 CIE_illum_D75 \
         CIE_illum_Dxx_comp CIE_illum_FLs CIE_illum_LEDs CIE_illum_HPs; do
  curl -fsSL -o data/cie/$f.csv https://files.cie.co.at/Publications-datasets/$f.csv
done
for f in KLUM_spectra.csv KLUM_quality.csv LICENSE.txt; do
  curl -fsSL -o data/klum/$f \
    https://raw.githubusercontent.com/rebeccailehag/KLUM_library/ad68196296cce2b5b62382f5a72554191056fa77/$f
done
(cd data && shasum -a 256 -c ../tools/data.sha256)   # Linux：sha256sum -c
make data && git diff --exit-code                    # 应与提交的生成物完全一致
```

CIE 数据表的目录见 <https://cie.co.at/data-tables>。

## 方法与参考文献

- 色度计算：CIE 015:2018 *Colorimetry*, 4th ed., doi:10.25039/TR.015.2018。配色函数为 CIE 1931 2° 标准色度观察者。
- CIE 日光（10000 K）：按 CIE 015:2018 的方法由 S0、S1、S2 合成。
- 普朗克辐射体：c2 = 1.4388 × 10⁻² m·K（ITS-90；H. Preston-Thomas, *Metrologia* 27(1):3–10, 1990, doi:10.1088/0026-1394/27/1/002）。
- sRGB：IEC 61966-2-1:1999。
- 色调映射：K. Narkowicz, “ACES Filmic Tone Mapping Curve”, 2016-01-06,
  <https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/>。
- 环境光估计：按积分球的关系用房间平均值估计，与 M. F. Cohen, S. E. Chen, J. R. Wallace, D. P. Greenberg, “A Progressive
  Refinement Approach to Fast Radiosity Image Generation”, *Computer Graphics* 22(4):75–84, 1988,
  doi:10.1145/378456.378487 中的环境项同理。
- 建材反射率：R. Ilehag, A. Schenk, Y. Huang, S. Hinz, “KLUM: An Urban VNIR and SWIR Spectral Library Consisting of
  Building Materials”, *Remote Sensing* 11(18):2149, 2019, doi:10.3390/rs11182149。

数据的处理细节（采样、平均、取舍）见 `tools/prep.c` 的文件头。

## 简化与已知限制

- 表面均按朗伯漫反射处理；纹理只调节反射率的明暗，不改变光谱形状。
- 环境光按积分球（房间平均）估计，对所有表面相同，不计遮挡，也不计多次反射造成的色偏。
- 光谱范围限于 400–700 nm，配色函数在此范围外的部分未计入。
- 不做白平衡，画面显示光源自身的颜色；超出 sRGB 色域的分量截为 0。
- 遮挡物按 1 px 栅格化。点光源与平行光是硬阴影，边缘有锯齿；两个物体之间很窄的缝漏过的光会呈断续的细线。
- 演示场景固定为 256 × 160 像素。南侧不砌墙，遮挡计算中同样开敞；窗洞内显示的窗外景色是风格化的画，不参与计算。
- 同一个引擎不能同时在两个线程里使用；不同的引擎之间互不影响。

## 嵌入网页

```html
<link rel="stylesheet" href="<base>/demo.css">
<div class="spl-demo" data-base="<base>/"></div>
<script src="<base>/demo.js" defer></script>
```

`data-base` 指向 `engine.wasm` 与两个数据文件所在的目录。`engine.wasm` 需先用 `make` 编译，也可以直接取在线演示里的
<https://finnchou.github.io/spectral-pixels/engine.wasm>。界面有中文、英文、日文三种，默认中文，画面右上角可以切换；
`data-lang="en"` 或 `data-lang="ja"` 可让界面以英文或日文开始。界面颜色跟随宿主页面的 CSS 变量
（`--card-bg`、`--text-color`、`--text-muted-color`、`--main-border-color`），没有这些变量时使用浅色的后备值。

## 参与维护

欢迎提 issue 与 pull request。提交前请运行 `make format`、`make test`、`make sanitize` 与 `make check`。渲染结果的任何变化都应当是有意的：
`make check` 会把它当作差异报出；确认无误后用 `node tools/check.mjs --update` 更新 `tests/reference.txt`，并在说明里写明哪些输出变了、为什么变。
接口的变动记入 `CHANGELOG.md`。

## 许可

除下列数据文件外，本仓库的内容按 [MIT](LICENSE) 发布，© 2026 FinnZhou：可以自由使用、修改、再发布，包括商用，
只需保留版权声明与许可文本。

- `src/cmf.h`、`docs/illuminants.txt` 含 CIE 数据，按 [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) 发布。
  由 `src/spl.c` 编译出的程序（`libspl.a`、`engine.wasm`、`render` 以及你自己的程序）都编入了 `cmf.h`，这部分数据仍受
  CC BY-SA 4.0 约束，再发布时须保留 CIE 的署名与许可链接。
- `docs/reflectances.txt` 含 KLUM 数据，按 GPL-2.0 发布，许可证全文见 [`docs/GPL-2.0.txt`](docs/GPL-2.0.txt)。
  该文件是纯文本，本身即为源码形式，可由 `tools/prep.c` 从上游固定提交的数据复现。

# Third-party notices / 第三方组件声明

This file lists the third-party components that are bundled with this
project, or were used to generate parts of it. **Their licences do not
change the MIT licence of the firmware source itself** — see
[LICENSE](LICENSE).

本文件列出随本项目分发、或用于生成本项目部分内容的第三方组件。
**这些组件的授权不影响固件源码本身的 MIT 授权** —— 见 [LICENSE](LICENSE)。

---

## 1. Fonts / 字体

The glyph data embedded in the firmware is generated from the following
two fonts. Both are released under the **SIL Open Font License 1.1**,
which permits embedding and subsetting into a product on condition that
the licence text is distributed with it. That is why both OFL texts ship
in this repository and in every release package.

固件内嵌的字形数据由以下两款字体生成，均以 **SIL Open Font License 1.1**
授权。该授权允许把字体嵌入、子集化到产品中，条件是随产品一并分发授权文本 ——
这就是两份 OFL 文本随本仓库与每个发布包一起分发的原因。

| Font | Upstream project | Licence text |
|---|---|---|
| Ark Pixel 12 px | `TakWolf/ark-pixel-font` | [`OFL-1.1-Ark-Pixel.txt`](OFL-1.1-Ark-Pixel.txt) |
| Source Han Sans SC (思源黑体) | `adobe-fonts/source-han-sans` | [`OFL-1.1-Source-Han-Sans.txt`](OFL-1.1-Source-Han-Sans.txt) |

No font file is redistributed here. Only subsetted glyph outlines are
embedded in the firmware binary; the original TTFs are not part of this
repository.

本仓库不分发字体文件本身，只在固件二进制中嵌入子集化的字形轮廓；
原始 TTF 文件不属于本仓库。

---

## 2. Station data / 电台数据

This firmware bundles **no station addresses and no playlist data**
(`g_station_count == 0`). Everything you listen to comes from a list you
imported yourself.

Station names, stream URLs and the audio they carry belong to their
respective rights holders. This project does not distribute, mirror or
warrant them, and takes no position on the legality of any particular
stream in any particular jurisdiction. **Judging that is your own
responsibility** — see the disclaimer in [README.md](README.md).

本固件**不内置任何电台地址或台单数据**（`g_station_count == 0`），
你听到的一切都来自你自己导入的清单。

你导入的台名、直播流地址与音频内容，权利归各自权利人所有。本项目不作分发、
不作镜像、不作担保，也不对任何特定流在任何司法辖区的合法性表态。
**这需要你自行判断** —— 见 [README.md](README.md) 的免责声明。

---

## 3. Build dependencies / 编译依赖

Built with [Espressif ESP-IDF](https://github.com/espressif/esp-idf)
(Apache-2.0) and [LVGL](https://github.com/lvgl/lvgl) (MIT). Both are
pulled in as build dependencies and are not vendored in this repository.

基于 [Espressif ESP-IDF](https://github.com/espressif/esp-idf)（Apache-2.0）
与 [LVGL](https://github.com/lvgl/lvgl)（MIT）构建；两者均作为编译依赖引入，
未随本仓库分发。

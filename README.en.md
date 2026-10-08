# XianDial Suite · 拾声集

**A 3.5-inch ESP32-S3 desktop device that carries four things in one firmware:
an internet radio, a Chinese fortune-stick oracle, a weather clock, and a stock ticker.**

> 🌐 **中文版: [README.md](README.md)** · Flashing: [FLASHING.en.md](FLASHING.en.md) · [FLASHING.md](FLASHING.md)
>
> The name is 拾声集 — "gathering of sounds" + "collection / suite".
> Built on the same base as the open-source **XianDial** radio: one set of display,
> touch, font, audio and power code. The four apps only swap the UI and data layers,
> so they do **not** need four separate devices.

---

## The four apps

The home screen is a 2×2 grid. Tap a card to enter; each app tears its own screen
down on exit, so switching back and forth does not accumulate memory.

| # | App | What it does |
|---|---|---|
| 0 | **XianDial Radio** | Live internet radio, filterable by 13 categories / 42 regions, plus local audio playback from the SD card, with a real-time spectrum |
| 1 | **Guanyin Oracle** | The traditional 100-stick Guanyin oracle: recite the Heart Sutra → choose your question → shake → reveal the verse (vertical layout) → a five-layer reading. The verse can be read aloud |
| 2 | **Weather Clock** | Live weather with a **72 px clock**, date, humidity / air quality / PM2.5, and a 3-day forecast. Optional voice announcement |
| 3 | **Stock Ticker** | Your watchlist with live change (red = up, green = down, the Chinese convention), intraday chart, and daily candles with MA5/10/20 |

**The oracle's fortune and palace distributions follow the classical text** —
22 upper / 60 middle / 18 lower, with unequal palace lengths, not the
re-arranged "internet edition" that circulates widely.

---

## ★★ Read this first: two build variants

This repository ships in the **release** configuration. The station list is the
only difference between the two variants:

| | **Release** (default here; used when `main/net_stations_pub.c` exists) | **Personal** |
|---|---|---|
| Built-in stations | **0** | **1254** (counted on-device) |
| Station-name font | `fonts/xs_font_pub_st16.c` (GB2312 level-1, 3755 glyphs, 2 bpp) | `fonts/xs_font_st16.c` (776 glyphs baked from that machine's list, 4 bpp) |
| Favourites seeding | Off (`XS_FAV_PUBLISH=1`) | Seeds 10 local favourites |
| Firmware size | 5,588,592 B | 4,734,400 B |

**Why the release build ships clean:** the station list is not my content — it is an
**index**. Shipping it inside the firmware means rebroadcasting on someone else's
behalf, and parts of that list were only obtained by testing from my own machine.
⇒ The release build carries an empty list. **Your stations are pushed by you to your
own device** (next section).

> ★ There is exactly **one** way to tell which variant a given binary is:
> count the station names inside `build/lingqian-radio.bin`.
> When both station files are present, CMake silently takes the release branch —
> static source scanning cannot detect this.

---

## Adding your own stations: three steps

### 1. Prepare a TSV

Four columns, **TAB**-separated, UTF-8 without BOM, LF line endings:

```
name	category	region	url
上海戏曲广播	戏曲	上海	http://lhttp.qtfm.cn/live/5054/64k.mp3
中国之声	新闻综合	全国	http://satellitepull.cnr.cn/live/wxzgzs/playlist.m3u8
```

- **category** must be one of 13 exact names:
  `新闻综合 交通台 音乐 文艺 说书 戏曲 怀旧老歌 网络台 教育台 电视伴音 综合 宗教 境外新闻`
- **region** must be one of 42 exact names (31 mainland provinces, plus
  中国台湾 / 中国香港 / 中国澳门 and 其他华语 / 北美 / 欧洲 / 日韩 / 新马 / 东南亚 / 大洋洲 / 海外中文),
  or `全国` for stations with no regional affiliation
- **url** may be plain HTTP, HTTPS, or **HLS** (`.m3u8`)

### 2. Push it to the SD card (`/sdcard/stations.tsv`)

- Use `tools/_sd_push.py` (pushes line by line over serial, with read-back
  verification and `st_reload`), or
- Pull the card and copy the file with a card reader

You can verify on-device over serial:

```
st_ls          # is stations.tsv on the card, and how big
st_reload      # re-read the list without rebooting
```

### 3. Keep station names from rendering as boxes

The station-name font is subsetted from the glyphs actually used. If your list
contains rare or traditional characters, re-bake it:

```bash
python gen_fonts_st.py --proj=lingqian-radio --only=16 --ui-font=<your kai ttf>
# then remove main/net_stations_pub.c so CMake takes the personal branch
```

> Why there are two station-name fonts, and why the full CJK set is not an option:
> see the "Fonts" section below.

---

## Hardware specification

Measured on the author's own unit. **This firmware has only been verified on this
hardware** — a different panel, touch controller or audio path requires code changes.

| Item | Spec |
|---|---|
| MCU | **ESP32-S3** (512 KB SRAM / **8 MB PSRAM** / 16 MB flash) |
| Display | **3.5" IPS TFT**, **320×480 portrait**, ST77922, QSPI |
| Touch | FT6336G capacitive (I²C) |
| Audio | ES8311 codec → FM8002E amplifier, mono speaker (`PA_EN` = IO1, active low) |
| Storage | On-board **TF card slot** (SDIO, 4-bit) |
| Power | 3.7 V Li-ion + LDO; deep-sleep shutdown (wake on BOOT button) |
| Framework | ESP-IDF 6.1 + LVGL 9 |
| Partitions | 7 MB app partition, **no OTA** — upgrades need a reflash |

**Portrait is the native orientation, not a rotation**: the panel itself is 320×480,
display flush does zero rotation, and touch coordinates satisfy `raw == lv`.
If the orientation is wrong in your enclosure, change one macro in `main/app_pins.h`:

```c
#define LQ_PORTRAIT_FLIP 0      /* change only this; never flip display and touch separately */
```

> ★ **Do not flip the display and the touch separately** — that produces
> "taps land on the opposite side" and no corner calibration ever fixes it.

---

## Voice files (put them on the SD card)

| File | Purpose |
|---|---|
| `/sdcard/lq001.mp3` … `lq100.mp3` | Verse recitation for the 100 oracle sticks (~26 s each) |
| `/sdcard/ommani.mp3` | Cover background audio (Om Mani Padme Hum, looping) |
| `/sdcard/va/*.mp3` | Voice atoms for the weather announcement (optional) |

**Missing files do not prevent boot** — those features are simply silent.

---

## Privacy (this section is a commitment, not marketing copy)

### What this device does on the network

Only two of the four apps use the network, and every endpoint is **fixed,
hard-coded, and verifiable in this repository**:

| Purpose | Endpoint | Notes |
|---|---|---|
| Weather | `t.weather.itboy.net` | live weather, **GET only** |
| Stock quotes | `qt.gtimg.cn` | watchlist quotes, **GET only** |
| Stock candles | `money.finance.sina.com.cn` | intraday + daily, **GET only** |
| Time sync | `ntp.aliyun.com` | SNTP |

**Beyond those, the only host contacted is whatever station you add yourself**
(the release build ships with an empty list).

### Verify it yourself

```bash
# Every fixed outbound domain in the firmware
grep -rhoE "http://[A-Za-z0-9._-]+" main/*.c main/*.h | sort -u

# Confirm there are no POST / PUT requests (only GET should appear)
grep -rn "HTTP_METHOD_POST\|HTTP_METHOD_PUT" main/
```

### What is stored on the device

- **NVS** (`xs_cfg` namespace): volume, backlight, mute, and WiFi credentials for
  provisioning
- **SD card**: the station list and audio you push
- No accounts, no telemetry, no usage data ever leaves the device

---

## Building and flashing

See **[FLASHING.en.md](FLASHING.en.md)**. Key points:

- Requires **ESP-IDF 6.1**
- Flashing the merged image is easiest; otherwise write the three parts separately

| File | Offset |
|---|---|
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `lingqian-radio.bin` | `0x10000` |

> ⚠️ **Never flash `lingqian-radio.bin` alone at `0x0`** — that writes the application
> into the bootloader region and the board will not boot.

---

## Fonts

The UI uses **LXGW WenKai** (SIL OFL 1.1 — permitted for firmware embedding and subsetting).

**Four regular sizes plus one special-purpose face**:

| Size | Used for |
|---|---|
| 14 px | captions |
| 18 px | card titles, forecast rows, buttons |
| 22 px | weather words, oracle reading text, city, date |
| 30 px | home title, oracle verse, large temperature |
| **72 px** | **clock only** (`xs_font_clk72`, 12 glyphs: `0-9 : -`, 55.5 KB) |

> ★ **Why the clock gets its own face**: the main font has 2081 glyphs; taking all of
> them to 72 px would need ~16.6 MB, and the app partition is only 7 MB.
> The clock is always `%02d:%02d`, so only those 12 glyphs are baked — a **500× difference**.
> This is the normal way to split a font by purpose: when a large size only serves a
> fixed phrase, bake just that phrase.

**Station-name fonts** are separate (see the "two build variants" table):
the release build uses the full GB2312 level-1 set (generic, builds out of the box),
while the personal build is subsetted from that machine's own station list
(smaller, tighter fit).

---

## Debug commands (serial)

The board only has BOOT / RESET buttons, so a full set of serial commands exists
for verification:

| Command | Effect |
|---|---|
| `app_home` | Return to the home grid |
| `app_open <0-3>` | Launch app n (0=Radio 1=Oracle 2=Weather 3=Stocks) |
| `lq_demo <page\|set> [n]` | Jump to an oracle page |
| `lq_play <n>` | Play verse audio n |
| `lq_shot` | Dump the current framebuffer to the PC (via `tools/_lq_shot.py` / `_lq_ui.py`) |
| `radio <view\|tap\|scroll\|filter\|cattab\|sd>` | Radio page: view / tap row / scroll / filter / switch tab / enter folder |
| `ui_demo <cfg\|cfg <y>\|cfgclose\|prov\|dump\|speak\|add\|power>` | Settings panel / provisioning / object-tree dump / announce / add stock / power key |
| `st_put` / `st_reload` / `st_ls` | Push stations / reload / inspect card |
| `lq_mem` | Memory and LVGL pool usage |

---

## What is — and is not — in this repository

**Included**: source for all four apps, fonts, cover artwork, generator scripts,
flashing docs, privacy notes.

**Deliberately excluded**:

- **Station lists** (`main/net_stations.c` and the personal station-name fonts
  `main/fonts/xs_font_st*.c`) — see "two build variants"
- **The 100 oracle voice MP3s** — large; supply your own (the app still runs without them)
- Any credentials, tokens or personal paths

---

## Known limitations

- **No OTA**: upgrading requires a reflash (no OTA slot in the partition table)
- Radio streams must be plain HTTP, HTTPS, or HLS. Internal RAM's largest contiguous
  block is only ~31 KB, while a TLS handshake needs 16–40 KB **contiguous** ⇒
  **stations that require TLS will not play** (a hard memory limit, not a bug)
- Rare / traditional characters in **station names** require re-baking the font
  (otherwise they render as boxes)
- Weather and market data come from third-party public endpoints and are provided
  **without any guarantee of availability or accuracy**

---

## Disclaimer

- This is a **personal, educational** open-source hardware project.
- All station URLs are **publicly accessible stream addresses**. This project does not
  host, relay or store any audio content. If you are a rights holder and do not want an
  address listed, please open an issue and it will be removed.
- The Guanyin oracle texts come from the **classical, public-domain edition**. This
  project only presents them digitally and **does not promote superstition**; the
  readings are cultural interpretation and **do not constitute advice of any kind**.
- Weather and stock data come from third-party public endpoints and are
  **for reference only — do not use them for trading decisions**.
- The author accepts no liability for any direct or indirect loss arising from the use
  of this firmware.

---

## License and credits

**MIT License** — see [LICENSE](LICENSE).
Third-party components and font licences: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

- UI font: **LXGW WenKai** — SIL OFL 1.1
- Fallback font: **Source Han Sans SC** — SIL OFL 1.1
- Frameworks: **ESP-IDF**, **LVGL**

© 2026 永远的兰兰 · Lanlan Eternal

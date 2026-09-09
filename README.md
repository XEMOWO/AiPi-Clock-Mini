<div align="center">

# ⏰ AiPi-Clock-Mini

<img src="images/clock-banner.svg" alt="AiPi-Clock-Mini" width="860">

**Desktop Clock on Ai-WB2 (BL602)**

RISC-V · ST7789 **172×320** LCD · LVGL 8 · WiFi + SNTP

[![Language](https://img.shields.io/badge/Language-C-blue?style=flat-square&logo=c)](.)
[![Chip](https://img.shields.io/badge/Chip-Ai--WB2_(BL602)-00a4e4?style=flat-square)](.)
[![Screen](https://img.shields.io/badge/Screen-ST7789_172x320-22c55e?style=flat-square)](.)
[![UI](https://img.shields.io/badge/UI-LVGL_8-e91e63?style=flat-square)](.)
[![Network](https://img.shields.io/badge/Network-WiFi%2BSNTP-8b5cf6?style=flat-square)](.)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%E2%80%A2%20Linux%20%E2%80%A2%20WSL-0ea5e9?style=flat-square)](.)
[![Release](https://img.shields.io/badge/Release-v0.0.2-22d3ee?style=flat-square)](https://github.com/XEMOWO/ai-wb2-clock/releases)

[![English](https://img.shields.io/badge/English-0ea5e9?style=for-the-badge)](README.md) [![中文](https://img.shields.io/badge/%E4%B8%AD%E6%96%87-ef4444?style=for-the-badge)](README.zh-CN.md)

</div>

---

An LVGL desktop clock built on **Ai-WB2 (BL602)**: out of the box it shows the
time (NTP-synced) and connects to WiFi via phone provisioning — the weather,
wallpaper playback, status lamp and the PC companion app are **built-in but
disabled by default** (second-dev scope, see below).

### ✨ Features

**🎉 Factory default (flash-and-go — nothing else to configure):**

| What | Details |
|------|---------|
| 🖥️ Display | Full-color **172×320** LCD over **hardware SPI @ 40 MHz**; GUI Guider designed UI on **LVGL 8** |
| 📶 WiFi | **No WiFi preloaded** — first boot goes straight into *phone provisioning*: open the open AP `Clock-Mini-XXXX`, then `192.168.4.1` in the browser |
| ⏰ Time | NTP / SNTP **automatic time sync** once connected; clock UI with scroll animation & gradient background |
| ⌨️ Commands | xcmd UART commands — change WiFi / city on the fly |
| 💾 Storage | easyflash persistence — settings survive power-off |
| 🪟 Flashing | One-click Windows flashing (zero install); Linux / WSL / MSYS2 supported |

**🛠️ Optional (built in, OFF by default — second-dev scope).**
These ship in the firmware but are disabled until you switch them on (most are
driven from the [PC companion app](#-pc-companion-app-wb2serialtool)):

| Feature | Factory state | How to enable |
|---------|---------------|---------------|
| 🌦️ Weather (icons + temperature / humidity) | **off** (`wx_on = 0`) | set `wx_on = 1` in `WB2-clock2/xcmd/cfg_store.c` and re-flash — the Gaode (amap) key is already pre-loaded; switch provider / key afterwards via `#XWXAPI` / `#XWXKEY` |
| 🖼️ Wallpaper playback | **off** (`wall_mode = 0`) | companion app “Wallpaper” card (uploads image over UART, sets wallpaper/clock display duration & animation) |
| 🏞️ Boot image | **none** (`wall_valid = 0`) | companion app “Boot image” card (full-screen 320×220, shows 2 s then clock) |
| 💡 Status lamp / monitor | **off** (`mon_on = 0`) | `#XLAMP,0-4` and `#XMON,1` serial commands |
| 🎨 Clock style & background | scroll animation **on**, blue-purple gradient | `#XCLOCK` / `#XBG` commands or the app’s color picker |
| 🖥️ PC companion app | separate app — **not firmware-dependent** | run `serial_tool/dist/WB2SerialTool.exe`; see below |

### 🖥️ PC Companion App (WB2SerialTool)

The PC software used with this project is **`serial_tool/dist/WB2SerialTool.exe`** —
a single-file Windows GUI app, **no installation required**, run it directly.
It talks to the board over UART (same protocol as the firmware's `xcmd`):

| Capability | Description |
|-----------|-------------|
| 📡 Serial | Port / baud setup (default **2 Mbps**), connection status |
| 📶 WiFi | Push SSID / password / city on the fly |
| 🌦️ Weather | Set weather API provider (Gaode / QWeather) and keys |
| 🖼️ Wallpaper | Upload images over UART (no flasher needed), set playback animation |
| ⏱️ Timing | Independent display seconds for **wallpaper** and **clock** (1–60 s) |
| 🎨 Background | Color picker for clock-screen background + gradient direction (solid / horizontal / vertical) |
| 🔘 Quick | One-click command buttons (ping / status / version) |

Settings persist to `config.json` next to the exe. Source lives in
[`serial_tool/`](serial_tool/) (PySide6 + qfluentwidgets); rebuild with
`build.bat` (PyInstaller).

> 💡 Everything in the app can also be sent by hand — see
> [Serial Protocol](#-serial-protocol-xcmd) below.

### 🔌 Wiring

| ST7789 | WB2 Pin | | ST7789 | WB2 Pin |
|--------|---------|---|--------|---------|
| SCL (SCK) | **GPIO 3** | | VCC | **3.3V** |
| SDA (MOSI) | **GPIO 12** | | GND | **GND** |
| CS | **GPIO 14** | | DC | **GPIO 4** |
| RST | **GPIO 17** | | — | — |

> ⚠️ Wrong pins won't damage the board — but never reverse **VCC/GND**.

### 🚀 Quick Start

**Method 1 — Windows one-click (zero install)**

1. Get this repo: green **Code → Download ZIP** (or `git clone`), then extract
2. Open `Windows烧录/` → double-click `一键烧录.bat` *(flash tool, prebuilt firmware & 70+ flash configs all inside the repo — nothing else to install)*
3. Enter the COM port (e.g. `COM3` from Device Manager)
4. When prompted: **hold BOOT → tap RST → release BOOT**
5. Done — it flashes automatically!

> This repo is self-contained for flashing — **no SDK, no compiler**. Only the CH340 driver is needed (usually auto-installed on Win10+).

**Method 2 — Linux / WSL script** *(requires the SDK, same as Method 3; just flashing → Method 1)*

```bash
./flash.sh                     # build → pick port → pick baud → flash
./flash.sh /dev/ttyUSB0 921600 # no interaction
./flash.sh --no-sync           # code-only changes, skip UI sync
```

> Serial permission (once): `sudo usermod -aG dialout $USER`
> If flashing stalls: **hold BOOT → tap RST** to re-enter download mode.

**Method 3 — build from source** *(requires the SDK — see the expanded section below)*

```bash
git clone https://github.com/XEMOWO/AiPi-Clock-Mini
cd AiPi-Clock-Mini

make -j8                       # build only
make flash SERIAL_PORT=/dev/ttyUSB0 SERIAL_BAUDRATE=921600
```

> ⚠️ **This repo does NOT contain the SDK** — no `components/`, `toolchain/` or `make_scripts_riscv/`. Compiling requires an external SDK; follow the expanded section below. Only want to flash? Use **Method 1** — nothing to install.

<details>
<summary><b>🧰 Set up a custom SDK environment</b> (click to expand)</summary>

> **This repo ships source only — no SDK.** Building requires an external [Ai-Thinker-WB2 SDK](https://gitee.com/Ai-Thinker-Open/Ai-Thinker-WB2); the steps below set it up once:

1. **Clone the SDK + submodules**

```bash
git clone https://gitee.com/Ai-Thinker-Open/Ai-Thinker-WB2
cd Ai-Thinker-WB2
git submodule update --init --recursive    # toolchain + flash tool — required!
```

2. **Apply a required patch** (otherwise colors are all wrong)

GUI Guider exports RGB565 little-endian images, but the SDK defaults to `LV_COLOR_16_SWAP=1`:

```bash
sed -i 's/#define LV_COLOR_16_SWAP 1/#define LV_COLOR_16_SWAP 0/' \
    components/stage/lvgl/lv_conf.h
```

3. **Place the project** (either way)

```bash
# A (recommended): inside the SDK's applications dir — path auto-detected
cp -r WB2-clock2 applications/xemowo/

# B: anywhere, point to the SDK manually
export BL60X_SDK_PATH=/path/to/Ai-Thinker-WB2
```

> SDK path resolution: env `BL60X_SDK_PATH` → parent-dir auto-detect → Makefile fallback.

4. **Flash**: chip **BL602**, flash size **2M**, file `build_out/WB2-clock2.bin`.

</details>

### 📁 Project Structure

```
WB2-clock2/
├── Makefile              # Build entry (auto SDK path resolution)
├── proj_config.mk        # Chip/feature config (2M flash, WiFi, LVGL…)
├── flash.sh              # One-click: sync UI + build + flash
├── sync_gui.sh           # GUI Guider → project sync (UI devs only)
├── Windows烧录/           # Windows zero-install flashing (self-contained)
├── serial_tool/          # PC companion app (PySide6 source + exe)
│   ├── dist/WB2SerialTool.exe  # ⭐ The PC software you actually run
│   ├── main.py           # App entry (PyInstaller target)
│   ├── app/              # Serial, protocol, WiFi/weather/wallpaper widgets
│   └── build.bat         # Rebuild exe on Windows
└── WB2-clock2/           # Source (dir name must match outer dir)
    ├── main.c            # Entry: LCD(hardware SPI @ 40 MHz) + LVGL + WiFi + SNTP
    ├── custom/           # ⚠ Hand-written code, never synced by GUI Guider
    │   ├── custom.c/h    # Business logic (widgets, weather text/icon map)
    │   ├── weather.c     # Weather task: Gaode (HTTP) / QWeather (HTTPS+gzip) / Open-Meteo
    │   ├── gz.c/h        # Minimal gzip inflate (QWeather forces gzip)
    │   └── lv_font_cn.c  # Chinese font
    ├── generated/        # GUI Guider output (LVGL 9→8 converted)
    ├── assets/           # Fonts / images
    └── xcmd/             # UART commands + easyflash config storage
```

### ⚙️ Configuration

| Item | Default | Where to change |
|------|---------|-----------------|
| WiFi SSID | 出厂无默认 (首次开机自动进配网) | `DEFAULT_SSID` in `WB2-clock2/xcmd/cfg_store.c`, or live via xcmd |
| WiFi password | 出厂无默认 | `DEFAULT_PWD` |
| City (weather) | `440306` Shenzhen Bao'an | `DEFAULT_CITY` |
| Weather provider | `amap` | `DEFAULT_WXAPI` in `cfg_store.c`, or `#XWXAPI` over UART |
| UART (log/xcmd) | 2 Mbps 8N1 | hardware connection |

> Config is stored in flash (easyflash) and survives reboot.

### 🌦️ Weather API (3 sources)

> 🚨 Weather is **off by default** (`wx_on = 0` in `WB2-clock2/xcmd/cfg_store.c`).
> Set it to `1` and re-flash first, then everything below applies (the amap key
> is pre-loaded). See [Optional](#-features) above.

Three providers, switchable live over UART (`#XWXAPI`); API keys are sent by the host over UART (`#XWXKEY`) and stored in flash:

| Provider | Notes |
|----------|-------|
| `amap` (Gaode, default) | plain HTTP :80, city = adcode or Chinese name |
| `qweather` (QWeather) | HTTPS + TLS (SDK mbedtls, no CA verify) + on-device gzip decode |
| `openmeteo` (Open-Meteo) | **no API key needed**, built-in, works out of the box |

QWeather first resolves the city to a LocationID via `geoapi` (adcode **or** Chinese name both work — existing city config needs no migration), then queries the `now` API (auth via `X-QW-Api-Key` header). Weather text and icons reuse the same mappings as Gaode — no UI changes needed. Open-Meteo needs no key at all — pick it when you have none.

### ⌨️ Serial Protocol (xcmd)

Lines start with `#X`; value fields are UTF-8 bytes hex-encoded (HEX); every command acks with `#XA,<OK|ERR>,<CMD>[,hex data]`:

| Command | Meaning |
|---------|---------|
| `#XVER` | firmware version |
| `#XST` | status → `#XST,<state>,<ssid>,<ip>,<rssi>,<time>,<city>,<wxapi>` |
| `#XCFG,<hex ssid>,<hex pwd>,<hex city>` | set config (empty field = keep) |
| `#XCITY,<hex city>` | change city, triggers instant weather refresh |
| `#XWXCITY,<hex name>` | resolve city name → weather LocationID |
| `#XMON,<0\|1>` | Claude Code status-light monitor toggle |
| `#XLAMP,<0-4>` | Claude Code status light |
| `#XWXAPI,<hex "amap"\|"qweather"\|"openmeteo">` | switch weather provider (**3 sources**) |
| `#XWXKEY,<hex provider>,<hex key>[,<hex cred>]` | set API key (qweather: key + optional legacy credential id) |
| `#XCLOCK,<0\|1>` | clock page-turn animation toggle |
| `#XBG,<hex c1>,<hex c2>,<dir>` | clock background color / gradient direction |
| `#XWALL,<mode>,<sec>,<anim>,<clksec>` | wallpaper slideshow config (on / sec / anim / clock sec) |
| `#XIMGSTART,<total>` `#XIMG,<seq>,<len>,<hex>,<chk>` `#XIMGEND` | upload image over UART (chunked, hex+checksum) |
| `#XBOOTIMG` | upload / clear custom boot logo |
| `#XBOOTCLR` | clear boot counter |

Device → PC: every command acks with `#XA,<OK|ERR>,<CMD>[,hex data]`; events push as `#XEV,<ev>[,data]` (e.g. `GOT_IP`, `WIFI_DISCONNECT`).

Example — switch to QWeather and set the API key:

```
#XWXAPI,716d656174686572
#XWXKEY,7177656174686572,3631363232643131626666633437366161313062333665303863306630663461
```

### 🧑‍💻 For Developers

- **`custom/` is never synced** by GUI Guider (disabled in the script) — edit it directly
- **Change UI**: edit in GUI Guider (LVGL 9) → `./sync_gui.sh` (auto LVGL 9→8 conversion) → `./flash.sh`
- **Code-only changes**: `./flash.sh --no-sync`
- GUI Guider project path: `SRC` variable at the top of `sync_gui.sh` (local-only)

### ❓ FAQ

| Problem | Solution |
|---------|----------|
| Flashing stuck waiting for device | **hold BOOT → tap RST** to re-enter download mode |
| Colors all wrong | `LV_COLOR_16_SWAP` must be **0** in `lv_conf.h` (see patch) |
| Cannot connect to WiFi | Check SSID/password (`cfg_store.c` defaults / xcmd) |
| `BL60X_SDK_PATH` errors | Check SDK path (see "custom SDK environment") |
| No COM port in Device Manager | Install **CH340 driver**, replug USB |
| `BFLB FLASH MATCH TYPE FAIL` | Log `readdata: b'xxxxxxxx'` → take first 6 hex digits (e.g. `5e4016`) → copy a `.conf` from `Windows烧录/utils/flash/bl602/` and rename it `<MODEL>_<first6>.conf` |

---

<div align="center">

**If you like it, give it a ⭐ Star!**

</div>

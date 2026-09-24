<div align="center">

# ⏰ AiPi-Clock-Mini

<img src="images/clock-banner.svg" alt="AiPi-Clock-Mini" width="860">

**基于 Ai-WB2 (BL602) 的桌面时钟**

RISC-V · ST7789 **172×320** 彩屏 · LVGL 8 · WiFi + SNTP

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

基于 **Ai-WB2 (BL602)** 的桌面时钟：出厂默认就是「显示时间」——手机配网连上
WiFi 后 SNTP 自动校时、LVGL 时钟界面。天气、壁纸、状态灯、上位机等扩展功能**已
内置但默认关闭**，属于二次开发范畴（见下方「可选」）。

### ✨ 特性

**🎉 出厂默认功能（烧录即用，什么都不用配）：**

| 功能 | 说明 |
|------|------|
| 🖥️ 屏幕 | **172×320** 全彩 LCD（硬件 SPI @ 40MHz），GUI Guider 设计的 **LVGL 8** 界面 |
| 📶 配网 | **出厂不预置 WiFi** —— 首次开机直接进入手机配网：连上无密码热点 `Clock-Mini-XXXX`，浏览器打开 `192.168.4.1` 填路由 WiFi |
| ⏰ 时间 | 连网后 **SNTP 自动校时**；时钟界面自带滚动翻页动画、渐变背景 |
| ⌨️ 命令 | xcmd 串口命令，在线改 WiFi / 城市 |
| 💾 存储 | easyflash 持久化，断电不丢 |
| 🪟 烧录 | Windows 一键烧录（零安装），支持 Linux / WSL / MSYS2 |

**🛠️ 可选（内置但默认关闭，二次开发范畴）。**
固件里已有这些代码，默认关闭；先在代码里开或由配套上位机开启（多数用
[上位机](#-pc-配套软件wb2serialtool) 驱动）：

| 功能 | 出厂状态 | 怎么开启 |
|------|---------|---------|
| 🌦️ 天气（图标 + 大气温湿度） | **关**（`wx_on = 0`） | 改 `WB2-clock2/xcmd/cfg_store.c` 里 `wx_on = 0` 为 `1` 后重新烧录——高德 API Key 已内置；之后可用 `#XWXAPI` / `#XWXKEY` 换天气源 / Key |
| 🖼️ 壁纸图播放 | **关**（`wall_mode = 0`） | 上位机「壁纸」卡片：串口上传图片，设壁纸/时钟各自显示时长与切换动画 |
| 🏞️ 开机图 | **无**（`wall_valid = 0`） | 上位机「开机图」卡片：全屏 320×220 上传，上电显示 2 秒后进时钟 |
| 💡 状态灯 / 监控 | **关**（`mon_on = 0`） | 串口命令 `#XLAMP,0-4`（状态灯）和 `#XMON,1`（监控开关） |
| 🎨 时钟样式 / 背景 | 滚动翻页**开**、蓝紫渐变 | `#XCLOCK` / `#XBG` 命令，或上位机背景颜色面板 |
| 🖥️ 上位机软件 | 独立软件，**与固件无关** | 直接运行 `serial_tool/dist/WB2SerialTool.exe`，见下节 |

### 🖥️ PC 配套软件（WB2SerialTool）

本项目配套的 PC 软件就是 **`serial_tool/dist/WB2SerialTool.exe`** ——
Windows 单文件 GUI，**免安装，直接双击运行**，通过串口和板子通信
（协议与固件 `xcmd` 完全一致）：

| 功能 | 说明 |
|------|------|
| 📡 串口 | 端口 / 波特率设置（默认 **2Mbps**）、连接状态 |
| 📶 WiFi | 在线下发 SSID / 密码 / 城市 |
| 🌦️ 天气 | 切换天气源（高德 / 和风）并下发 API Key |
| 🖼️ 壁纸 | 串口直接上传图片（无需烧录器），可选切换动画 |
| ⏱️ 时长 | **图片**与**时钟**各自独立的显示秒数（1–60 秒） |
| 🎨 背景 | 时钟屏背景颜色面板 + 渐变方向（纯色 / 水平 / 垂直） |
| 🔘 快捷 | 一键命令按钮（PING / 状态 / 版本） |

设置自动保存到 exe 同目录的 `config.json`。源码在
[`serial_tool/`](serial_tool/)（PySide6 + qfluentwidgets），
Windows 上跑 `build.bat` 即可用 PyInstaller 重新打包。

> 💡 软件里能发的命令，手工串口也能发——格式见下方
> [串口协议](#-串口协议xcmd)。

### 🔌 接线

| ST7789 屏 | WB2 引脚 | | ST7789 屏 | WB2 引脚 |
|-----------|---------|---|-----------|---------|
| SCL (SCK) | **GPIO 3** | | VCC | **3.3V** |
| SDA (MOSI) | **GPIO 12** | | GND | **GND** |
| CS | **GPIO 14** | | DC | **GPIO 4** |
| RST | **GPIO 17** | | — | — |

> ⚠️ 接错线不会烧板，但 **VCC/GND 千万别反接**。

### 🚀 快速开始

**方法一：Windows 一键烧录（零安装）**

1. 获取本仓库：绿色 **Code → Download ZIP**（或 `git clone`），然后解压
2. 打开 `Windows烧录/` → **双击 `一键烧录.bat`**（烧录工具、预编译固件、70+ 型号 flash 配置全都在仓库里，什么都不用装）
3. 输入串口号（设备管理器查看，如 `COM3`）
4. 提示时**按住 BOOT → 按一下 RST → 松开 BOOT**
5. 自动烧完，开机！

> 只烧录时本仓库是自包含的——**不需要 SDK、不需要编译器**，只需 CH340 驱动（Win10+ 一般自动装）。

**方法二：Linux / WSL 脚本**（同方法三；本机已有工具链就直接用，没有才自动拉；只想烧录用方法一）

```bash
./flash.sh                     # 编译 → 选串口 → 选波特率 → 烧录
./flash.sh /dev/ttyUSB0 921600 # 直接指定，无交互
./flash.sh --no-sync           # 只改代码，跳过 UI 同步
```

> 串口权限（一次）：`sudo usermod -aG dialout $USER`
> 卡住时：**按住 BOOT → 按 RST** 重新进入下载模式。

**方法三：从源码编译**（SDK 随仓库自带；工具链本机已有就直接用，没有才拉）

```bash
git clone https://github.com/XEMOWO/AiPi-Clock-Mini
cd AiPi-Clock-Mini

./flash.sh                     # 编译 → 选串口 → 烧录（确实缺工具链才会去拉）
```

> 不需要另外装 SDK：`sdk/` 目录里就是**打好全部补丁的 SDK 源码**。
>
> **本机已经装了 RISC-V 工具链的话，一个字节都不下载** —— 依次找
> `$CONFIG_TOOLPREFIX` → `$BL60X_TOOLCHAIN_PATH` → `PATH` → `/opt/riscv`、`~/riscv` 等常见目录。
> 找到的还会真编一个 `-march=rv32imfc -mabi=ilp32f` 的小程序验一下：像平头哥那种名字对、但只装了
> rv64 库的（链接到一半就 `ELFCLASS64 incompatible`），会被明确报出来跳过，不会编到一半才炸。
>
> 确实没有，才从官方镜像拉**当前平台那一份**工具链 + 烧录工具（约 2.3 GB，只拉一次，
> 带进度条和速率），不会把两个平台都拉下来。
>
> macOS：仓库不提供 macOS 版工具链，本机没有就直接报错说清楚，不会白拉 2.3 GB。

<details>
<summary><b>🧰 手动编译 / 用外部 SDK</b>（点击展开）</summary>

**不想用 flash.sh 时：**

```bash
git submodule update --init --recursive    # 首次: 拉工具链 + 烧录工具(约 2.3GB)
make -j8                                   # 编译
make flash SERIAL_PORT=/dev/ttyUSB0 SERIAL_BAUDRATE=921600
```

> 这一步只在**本机没有工具链**时才需要；有的话 `make` 会自动用本机的，找不到也会说清楚查到哪了。

烧录时芯片选 **BL602**，Flash **2M**，烧 `build_out/WB2-clock2.bin`。

**想用自己那套 SDK**（比如已经配好的开发环境）：

```bash
export BL60X_SDK_PATH=/你的/Ai-Thinker-WB2 路径
make -j8
```

> 路径解析顺序：环境变量 `BL60X_SDK_PATH` → 仓库自带 `sdk/` → 上级目录。都不匹配**直接报错**，不会拿错 SDK 编到一半才炸。

**`sdk/` 里打了哪些补丁** —— 用外部 SDK 的话要自己同步这些，否则各自的后果如下：

| 补丁 | 文件 | 不打的后果 |
|---|---|---|
| 导出固件大小符号 `_fw_size` | `flash.ld` `flash_rom.ld` | **链接失败**：`undefined reference to '_fw_size'` |
| FreeRTOS 堆 14100 → 40960 | `FreeRTOSConfig.h` | **和风天气全失败**：mbedtls 握手分配不出缓冲 |
| LVGL 色彩字节序 `LV_COLOR_16_SWAP` 1→0 | `lv_conf.h` | **图片颜色全错** |
| LVGL 刷新周期 30 → 16ms | `lv_conf.h` | 界面偏卡 |
| 串口接收缓冲下限提到 4096 | `vfs_uart.c` | 大帧丢数据 |
| WiFi 断线重连 AP-recover | `wifi_mgmr.c` | 掉线后不自愈 |
| 启动日志静音 + 极早期钩子 | `blog.c` `bfl_main.c` 等 10 处 | 开机黑屏期变长、上电计数失效 |

</details>

### 📁 项目结构

```
WB2-clock2/
├── Makefile              # 构建入口（SDK 路径自动解析）
├── proj_config.mk        # 芯片/功能配置（2M flash、WiFi、LVGL…）
├── flash.sh              # 一键: 同步UI + 编译 + 选串口/波特率 + 烧录
├── sync_gui.sh           # GUI Guider → 项目同步（仅 UI 开发者）
├── sdk/                  # ⭐ 打好全部补丁的 SDK 源码（工具链/烧录工具是 submodule，缺了才拉）
├── tools/find_toolchain.sh  # 工具链查找脚本（Makefile 和 flash.sh 共用同一份口径）
├── Windows烧录/           # Windows 零安装一键烧录（工具全内置）
├── serial_tool/          # PC 配套软件（PySide6 源码 + exe）
│   ├── dist/WB2SerialTool.exe  # ⭐ 日常使用的就是它
│   ├── main.py           # 程序入口（PyInstaller 打包目标）
│   ├── app/              # 串口/协议/WiFi/天气/壁纸面板
│   └── build.bat         # Windows 下重新打包 exe
└── WB2-clock2/           # 源码（目录名必须与外层同名）
    ├── main.c            # 入口: LCD(硬件SPI 40MHz) + LVGL + WiFi + SNTP
    ├── custom/           # ⚠ 手写代码，不随 GUI Guider 同步
    │   ├── custom.c/h    # 业务逻辑（控件定制、天气文字/图标映射）
    │   ├── weather.c     # 天气任务：高德(HTTP) / 和风(HTTPS+gzip) / Open-Meteo
    │   ├── gz.c/h        # 极简 gzip 解压（和风强制 gzip）
    │   └── lv_font_cn.c  # 中文字库
    ├── generated/        # GUI Guider 生成（LVGL 9→8 已转换）
    ├── assets/           # 字体/图片
    └── xcmd/             # 串口命令 + easyflash 配置存储
```

### ⚙️ 配置

| 配置项 | 默认值 | 改哪里 |
|--------|--------|--------|
| WiFi SSID | 出厂无默认（首次开机自动进配网） | `WB2-clock2/xcmd/cfg_store.c` 的 `DEFAULT_SSID`，或串口 xcmd 在线改 |
| WiFi 密码 | 出厂无默认 | `DEFAULT_PWD` |
| 城市（天气） | `440306` 深圳宝安 | `DEFAULT_CITY` |
| 天气源 | `amap` | `cfg_store.c` 的 `DEFAULT_WXAPI`，或串口 `#XWXAPI` 在线切 |
| 串口（日志/xcmd） | 2M bps 8N1 | 硬件连接 |

> 配置存 flash（easyflash），在线改完重启不丢。

### 🌦️ 天气 API（三源）

> 🚨 天气**出厂默认关闭**（`WB2-clock2/xcmd/cfg_store.c` 里 `wx_on = 0`）。
> 先改成 `1` 重新烧录，下面这些才生效（高德 Key 已内置）。详见上方
> [可选功能](#-特性)。

三个天气源，串口 `#XWXAPI` 在线切换；API Key 由串口 `#XWXKEY` 下发并存入 flash：

| 天气源 | 说明 |
|--------|------|
| `amap`（高德，默认） | 明文 HTTP :80，城市=adcode 或中文名 |
| `qweather`（和风） | HTTPS + TLS（SDK mbedtls，不校验证书）+ 板端 gzip 解压 |
| `openmeteo`（Open-Meteo） | **免 API Key**，内置，开箱即用 |

和风先用 `geoapi` 把城市反查成 LocationID（adcode **和**中文名都支持——现有城市配置无需迁移），再查 `now` 实时接口（认证走 `X-QW-Api-Key` 请求头）。天气文字和图标复用高德的映射，UI 无需改动。Open-Meteo 无需任何 Key，适合无 Key 场景直接使用。

### ⌨️ 串口协议（xcmd）

行以 `#X` 开头；值字段为 UTF-8 字节的 hex；每条命令回 `#XA,<OK|ERR>,<CMD>[,hex data]`：

| 命令 | 含义 |
|------|------|
| `#XVER` | 固件版本 |
| `#XST` | 状态 → `#XST,<state>,<ssid>,<ip>,<rssi>,<time>,<city>,<wxapi>` |
| `#XCFG,<hex ssid>,<hex pwd>,<hex city>` | 写配置（字段留空=不改） |
| `#XCITY,<hex city>` | 改城市，触发天气立即重查 |
| `#XWXCITY,<hex 城市名>` | 城市名反查天气 LocationID |
| `#XMON,<0\|1>` | Claude Code 状态灯监控开关 |
| `#XLAMP,<0-4>` | Claude Code 状态灯 |
| `#XWXAPI,<hex "amap"\|"qweather"\|"openmeteo">` | 切换天气源（**三源**） |
| `#XWXKEY,<hex provider>,<hex key>[,<hex cred>]` | 下发 API Key（和风：key + 可选旧版凭据ID） |
| `#XCLOCK,<0\|1>` | 时钟翻页动画开关 |
| `#XBG,<hex c1>,<hex c2>,<dir>` | 时钟背景颜色/渐变方向 |
| `#XWALL,<mode>,<sec>,<anim>,<clksec>` | 壁纸轮播配置（开关/时长/动画/时钟时长） |
| `#XIMGSTART,<total>` `#XIMG,<seq>,<len>,<hex>,<chk>` `#XIMGEND` | 串口分块上传图片（hex+校验） |
| `#XBOOTIMG` | 上传/清除自定义开机图 |
| `#XBOOTCLR` | 清零开机计数 |

设备 → 电脑：每条命令回 `#XA,<OK|ERR>,<CMD>[,hex data]`；事件主动推 `#XEV,<ev>[,data]`（如 `GOT_IP`、`WIFI_DISCONNECT`）。

示例——切到和风并下发 Key：

```
#XWXAPI,716d656174686572
#XWXKEY,7177656174686572,3631363232643131626666633437366161313062333665303863306630663461
```

### 🧑‍💻 开发者注意事项

- **`custom/` 不会也不该被 GUI Guider 同步**（脚本已禁用），直接编辑即可
- **改 UI**：GUI Guider（LVGL 9）改 → `./sync_gui.sh`（自动 LVGL 9→8 转换）→ `./flash.sh`
- **只改业务代码**：`./flash.sh --no-sync`
- GUI Guider 工程路径在 `sync_gui.sh` 顶部 `SRC` 变量（仅本机有效）

### ❓ FAQ

| 现象 | 解决 |
|------|------|
| 烧录卡在等待设备 | 按住 **BOOT** 再按 **RST**，重新进入下载模式 |
| 图片颜色全错 | 正常不会发生 —— 自带 `sdk/` 已内置 `LV_COLOR_16_SWAP 0`；用外部 SDK 才需要自己改 |
| 连不上 WiFi | 检查 SSID/密码（`cfg_store.c` 默认值 / xcmd 修改） |
| 报 `undefined reference to '_fw_size'` | 用了没打补丁的外部 SDK。删掉 `BL60X_SDK_PATH` 改用自带的 `sdk/`，或按上面补丁表自己同步 |
| 报「没找到能用的 riscv64-unknown-elf 工具链」 | `sh tools/find_toolchain.sh` 会列出查到哪、每个候选为什么不行。本机有就指明位置：`BL60X_TOOLCHAIN_PATH=<目录> make`；没有就让 `./flash.sh` 去拉自带的那份 |
| 设备管理器看不到串口 | 装 **CH340 驱动**，重新插拔 |
| 报 `BFLB FLASH MATCH TYPE FAIL` | 日志 `readdata: b'xxxxxxxx'` 取前 6 位（如 `5e4016`），在 `Windows烧录/utils/flash/bl602/` 复制一个 `.conf` 改名为 `<型号>_<前6位>.conf` |

---

<div align="center">

**喜欢的话点个 ⭐ Star 支持一下！**

</div>

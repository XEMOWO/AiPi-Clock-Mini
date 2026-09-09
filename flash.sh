#!/bin/bash
# ═══════════════════════════════════════════════════════════
# WB2-clock 一键: 同步UI + 编译 + 选串口/波特率 + 烧录
#
# 用法:
#   ./flash.sh                            # 全流程 (默认同步GUI代码)
#   ./flash.sh --no-sync                  # 跳过 GUI Guider 同步
#   ./flash.sh --no-build                 # 不编译, 只烧已有 bin
#   ./flash.sh /dev/ttyUSB0 921600        # 指定串口+波特率, 无菜单
#   组合: ./flash.sh --no-sync /dev/ttyUSB0
# ═══════════════════════════════════════════════════════════
set -e
cd "$(dirname "$0")"

# Windows (MSYS2) 检测: 串口是 /dev/ttyS*, 传给烧录工具要转成 COMx
IS_WIN=0
uname -s | grep -qiE 'mingw|msys' && IS_WIN=1

# ── 解析参数 ──────────────────────────────────────────────
SYNC=1
BUILD=1
ARGS=()
for a in "$@"; do
    case "$a" in
        --no-sync) SYNC=0 ;;
        --no-build) BUILD=0 ;;
        *) ARGS+=("$a") ;;
    esac
done
PORT="${ARGS[0]}"
BAUD="${ARGS[1]}"

TOTAL=3; [ "$SYNC" = 1 ] && TOTAL=4; [ "$BUILD" = 1 ] && TOTAL=$((TOTAL+1))
STEP=0
next() { STEP=$((STEP+1)); echo "════════ [$STEP/$TOTAL] $1 ════════"; }

# ── 0: GUI 代码同步 ───────────────────────────────────────
if [ "$SYNC" = 1 ]; then
    next "同步 GUI 代码 (GUI Guider)"
    ./sync_gui.sh
fi

# ── 1: 编译 ───────────────────────────────────────────────
if [ "$BUILD" = 1 ]; then
    next "编译中"
    make -j8
    echo "✔ 编译完成"
fi

# ── 2: 选串口 ─────────────────────────────────────────────
next "选择串口"
if [ -z "$PORT" ]; then
    # USB 转串口优先; 没有时再看 WSL 映射的 COM (ttyS*)
    PORTS=($(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true))
    if [ ${#PORTS[@]} -eq 0 ]; then
        PORTS=($(ls /dev/ttyS* 2>/dev/null || true))
    fi
    if [ ${#PORTS[@]} -eq 0 ]; then
        echo "⚠ 没检测到任何串口。"
        if [ "$IS_WIN" = 1 ]; then
            echo "  检查: 板子 USB 是否连上 / CH340 驱动是否装好"
            read -rp "  手动输入串口号 (如 COM3): " PORT
        else
            echo "  检查: 板子 USB 是否连上 / usbipd 是否转发"
            read -rp "  手动输入串口设备 (如 /dev/ttyUSB0): " PORT
        fi
    else
        echo "可用串口:"
        for i in "${!PORTS[@]}"; do
            echo "  $((i+1))) ${PORTS[$i]}"
        done
        echo "  0) 手动输入"
        read -rp "选择 [1]: " SEL
        if [ -z "$SEL" ] || [ "$SEL" = "1" ]; then
            PORT="${PORTS[0]}"
        elif [ "$SEL" = "0" ]; then
            read -rp "  输入串口设备: " PORT
        else
            IDX=$((SEL-1))
            [ "$IDX" -ge 0 ] && [ "$IDX" -lt "${#PORTS[@]}" ] || { echo "✘ 无效选择"; exit 1; }
            PORT="${PORTS[$IDX]}"
        fi
    fi
fi
# MSYS2 下把 /dev/ttySx 转成 Windows 串口名 COMx (ttyS0=COM1)
if [ "$IS_WIN" = 1 ] && [[ "$PORT" =~ ^/dev/ttyS([0-9]+)$ ]]; then
    PORT="COM$(( ${BASH_REMATCH[1]} + 1 ))"
fi
echo "✔ 串口: $PORT"

# ── 3: 选波特率 ───────────────────────────────────────────
next "选择波特率"
if [ -z "$BAUD" ]; then
    echo "波特率:"
    echo "  1) 115200"
    echo "  2) 460800"
    echo "  3) 921600   (默认, 烧录推荐)"
    echo "  4) 1500000"
    echo "  5) 2000000"
    echo "  0) 手动输入"
    read -rp "选择 [3]: " B
    case "$B" in
        ""|3) BAUD=921600 ;;
        1)    BAUD=115200 ;;
        2)    BAUD=460800 ;;
        4)    BAUD=1500000 ;;
        5)    BAUD=2000000 ;;
        0)    read -rp "  输入波特率: " BAUD ;;
        *)    echo "✘ 无效选择"; exit 1 ;;
    esac
fi
echo "✔ 波特率: $BAUD"

# ── 4: 烧录 ───────────────────────────────────────────────
next "开始烧录"
# 分区表以项目内为准, 覆盖 SDK 默认 (FW 单段合并, 支持 >864KB 固件)
SDK="${BL60X_SDK_PATH:-/home/xemowo/Ai-Thinker-WB2}"
cp Windows烧录/partition_cfg_2M.toml \
   "$SDK/tools/flash_tool/chips/bl602/partition/partition_cfg_2M.toml"
echo "提示: 如果卡住, 按住 BOOT 键再按一下 RST 重新进入下载模式"
make flash SERIAL_PORT="$PORT" SERIAL_BAUDRATE="$BAUD"
echo "✔ 烧录完成"

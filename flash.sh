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

# ── 拉 submodule (只在真的缺工具链时才走到) ───────────────
# git 默认只在 tty 上画进度条, 这里显式 --progress, 非 tty(CI/日志重定向)也能看到
# "Receiving objects: 45% ..., 12.3 MiB | 5.6 MiB/s" —— 有百分比也有速率。
# 浅克隆只拿得到分支 tip, 所以先比对"镜像 tip"和"仓库钉住的提交": 一致才敢用 --depth 1
# (少下历史, 快得多); 万一哪天镜像更新了就退回完整克隆, 不会卡死。
pull_submodule() {
    path="$1"; label="$2"
    url="$(git config -f .gitmodules --get "submodule.$path.url" 2>/dev/null || true)"
    pin="$(git ls-tree HEAD "$path" 2>/dev/null | awk '{print $3}')"
    tip="$(git ls-remote "$url" HEAD 2>/dev/null | cut -f1)"
    echo "── $label  ($path) ──"
    if [ -n "$pin" ] && [ "$pin" = "$tip" ]; then
        git submodule update --init --depth 1 --progress "$path" \
            || git submodule update --init --progress "$path" \
            || { echo "✘ $label 拉取失败, 检查网络后重试"; exit 1; }
    else
        echo "  (镜像 tip 已前进, 浅克隆取不到钉住的提交, 走完整克隆)"
        git submodule update --init --progress "$path" \
            || { echo "✘ $label 拉取失败, 检查网络后重试"; exit 1; }
    fi
}

# ── SDK 定位 + 工具链 ─────────────────────────────────────
# 仓库自带 sdk/ (源码已含全部补丁); 工具链与烧录工具是 submodule, 不在仓库里。
# 本机已经装了 riscv64-unknown-elf 就直接用, 一个字节都不下载; 真没有才拉,
# 而且只拉当前平台那一份 + 烧录工具 (不是 --recursive 把两个平台都拉下来)。
ROOT="$(pwd)"
SDK="${BL60X_SDK_PATH:-$ROOT/sdk}"
if [ ! -f "$SDK/make_scripts_riscv/project.mk" ]; then
    echo "✘ 找不到 SDK: $SDK"
    echo "  该目录应随仓库一起拿到; 确实没有的话用环境变量指定: BL60X_SDK_PATH=<你的 SDK 路径> ./flash.sh"
    exit 1
fi
export BL60X_SDK_PATH="$SDK"    # 让 make 用同一份, 免得两边解析出不同结果

if TOOLCHAIN_PREFIX="$(sh tools/find_toolchain.sh)"; then
    echo "✔ 用本机已装的工具链: ${TOOLCHAIN_PREFIX}gcc"
else
    # 本机没有, 只能拉仓库自带的那份 —— 它只覆盖 Linux 和 Windows(MSYS) 两个平台
    PLATFORM="$(uname -s | cut -d '_' -f1)"
    case "$PLATFORM" in
        MINGW*|MSYS*|CYGWIN*) PLATFORM=MSYS ;;
    esac
    if [ "$PLATFORM" != Linux ] && [ "$PLATFORM" != MSYS ]; then
        echo "✘ 本机没有 riscv64-unknown-elf 工具链, 仓库自带的也只有 Linux / Windows 两份, 没有 $PLATFORM 版"
        echo "  先自己装一个再跑; 装好但不在 PATH 上的话: BL60X_TOOLCHAIN_PATH=<工具链目录> ./flash.sh"
        exit 1
    fi
    if [ ! -d .git ]; then
        echo "✘ 本机没有工具链, 当前目录又不是 git 仓库(可能是解压出来的 ZIP), 没法自动拉"
        echo "  改用: git clone https://github.com/XEMOWO/AiPi-Clock-Mini"
        exit 1
    fi

    echo "════════ 本机没有工具链, 首次拉取 (约 2.3GB, 只需一次) ════════"
    pull_submodule "sdk/toolchain/riscv/$PLATFORM" "$PLATFORM 版工具链"
    pull_submodule "sdk/tools/flash_tool"          "烧录工具"

    # 拉完必须回头再验一次。刚下下来的东西光看"目录在不在"是不够的 ——
    # 仓库自带这份解出来没有可执行位, 不验就会一路走到 make 才炸, 那时日志已经看不出原因了。
    if TOOLCHAIN_PREFIX="$(sh tools/find_toolchain.sh)"; then
        echo "✔ 工具链就绪: ${TOOLCHAIN_PREFIX}gcc"
    else
        echo "✘ 拉下来了但不能用(上面 find_toolchain 说明了原因), 没法继续"
        exit 1
    fi
fi

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
cp Windows烧录/partition_cfg_2M.toml \
   "$SDK/tools/flash_tool/chips/bl602/partition/partition_cfg_2M.toml"
echo "提示: 如果卡住, 按住 BOOT 键再按一下 RST 重新进入下载模式"
make flash SERIAL_PORT="$PORT" SERIAL_BAUDRATE="$BAUD"
echo "✔ 烧录完成"

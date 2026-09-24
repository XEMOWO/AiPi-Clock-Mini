#!/bin/sh
# ═══════════════════════════════════════════════════════════
# 定位 riscv64-unknown-elf 工具链。
#
# Makefile 和 flash.sh 都调这一份, 保证"本机已有的直接用、确实没有才去拉"
# 这个判断在两处完全一致 —— 否则会出现 make 用本机的、flash.sh 却去拉 2GB。
#
# 成功: stdout 打印前缀(结尾带 '-', 拼上 gcc/ld/objcopy 就能用), 返回 0
# 失败: stdout 不输出, stderr 说明查到哪、为什么都不合适, 返回 1
#
# 查找顺序(先命中先用):
#   1. $CONFIG_TOOLPREFIX    SDK 自己的变量, 结尾带 '-' 的前缀 —— 明确指定, 直接信
#   2. $BL60X_TOOLCHAIN_PATH 工具链根目录, 或它的 bin/        —— 明确指定, 直接信
#   3. PATH                  which riscv64-unknown-elf-gcc
#   4. 常见安装位置            /opt/riscv, ~/riscv ...
#   5. 仓库自带 submodule      <repo>/sdk/toolchain/riscv/<平台>
#
# 3/4/5 是脚本自己猜的, 猜的就必须验一下(见 probe)。1/2 是用户明说的, 原样使用 ——
# 万一那份工具链不灵, 也该是它自己去报错, 而不是被这里悄悄换成别的。
#
# 注意 5: 仓库自带那份解出来是没有可执行位的(见 fix_perm), 所以判断"有没有"不能只看
# 文件在不在 —— 必须看起不起得来, 否则会一路判到"没找到", 而日志里完全看不出原因。
# ═══════════════════════════════════════════════════════════

set -u

# 仓库自带的那份在 sdk/toolchain/riscv/<平台>/。平台名按 uname 第一段取,
# 但 git-bash / MSYS2 下 uname 是 MINGW64_NT-*, 目录名却叫 MSYS, 这里归一。
PLATFORM="$(uname -s 2>/dev/null | cut -d '_' -f1)"
case "$PLATFORM" in
    MINGW*|MSYS*|CYGWIN*|Windows*) PLATFORM=MSYS ;;
esac

# Ai-Thinker SDK 自带的是 riscv64-unknown-elf; xPack 等第三方发行版叫 riscv-none-elf。
# 前者优先, 因为那是 SDK 官方验证过的那个。
NAMES="riscv64-unknown-elf riscv-none-elf"

# ── 补可执行位 ────────────────────────────────────────────
# 仓库自带那份是从 tar 包提交进 git 的, 入库时**可执行位全丢了**, 解出来是 -rw-r--r--。
# 上游 Ai-Thinker 同样如此, 所以包里专门带了个 chmod755.sh。它写的是相对路径
# (chmod 755 bin/xxx), 必须 cd 到工具链根目录再跑。
# 光靠"有没有 gcc 这个文件"判断会漏掉这种情况: 文件在、就是起不来。
#
# 变量一律带 _fp_ 前缀: POSIX sh 的函数**没有自己的作用域**, 里面写的每个变量都是全局的。
# 用光秃秃的 n 会顺手改掉调用方 resolve 的循环变量, 于是它补完权限回来复查时看的是
# 另一个文件名 —— 检查必然不过, 而文件其实已经修好了。这种错极难从表面看出来。
fix_perm() {
    _fp_b="$1"      # 放 gcc 的那个目录
    for _fp_root in "$_fp_b" "$_fp_b/.."; do
        if [ -f "$_fp_root/chmod755.sh" ]; then
            ( cd "$_fp_root" && sh chmod755.sh ) >/dev/null 2>&1
            return 0
        fi
    done
    # 没带脚本就至少把编译器等补上(_fp_b 就是 bin/, 别再往下拼一层)
    for _fp_n in $NAMES; do chmod +x "$_fp_b/$_fp_n-"* 2>/dev/null; done
    return 0
}

# gcc 在不在(不管能不能执行)。用来区分"什么都没有"和"有、但起不来" ——
# 这两种都得说清楚, 不然用户对着一句"没找到"根本没法下手。
# 同样带前缀, 理由见 fix_perm。
has_gcc() {
    for _hg_d in "$1" "$1/bin"; do
        for _hg_n in $NAMES; do
            [ -e "$_hg_d/$_hg_n-gcc" ] && return 0
        done
    done
    return 1
}

resolve() {
    cand="${1:-}"
    [ -n "$cand" ] || return 1

    for n in $NAMES; do
        case "$cand" in
            *"$n-")
                if [ -e "${cand}gcc" ] || [ -e "${cand}gcc.exe" ]; then
                    [ -x "${cand}gcc" ] || fix_perm "${cand%$n-}"
                    if [ -x "${cand}gcc" ] || [ -x "${cand}gcc.exe" ]; then echo "$cand"; return 0; fi
                fi
                ;;
        esac
    done

    for d in "$cand" "$cand/bin"; do
        for n in $NAMES; do
            if [ -e "$d/$n-gcc" ] || [ -e "$d/$n-gcc.exe" ]; then
                [ -x "$d/$n-gcc" ] || fix_perm "$d"
                if [ -x "$d/$n-gcc" ] || [ -x "$d/$n-gcc.exe" ]; then echo "$d/$n-"; return 0; fi
            fi
        done
    done
    return 1
}

# ── 验工具链的探针 ────────────────────────────────────────
# 光看名字不够: T-Head(平头哥)那份也叫 riscv64-unknown-elf-gcc, 但它只装了 rv64 的库,
# 拿 -march=rv32imfc -mabi=ilp32f 一链接就是 "ELFCLASS64 incompatible" —— 名字对、目标不对。
# 所以真去编一个 BL602 目标的小程序, 编得过才算数。约 0.1 秒。
# 探测不了(没有可写的临时目录)就当作通过 —— 这只是个筛子, 不该反过来挡路。
TB="$(mktemp -d 2>/dev/null)" || TB=""
if [ -n "$TB" ]; then trap 'rm -rf "$TB"' EXIT; fi

# 把候选规整成前缀并验证。命中就打印前缀并 exit 0(脚本就此结束);
# 解析得出但验证不过的, 记进 SKIPPED 继续往下找。
SKIPPED=""
MISSED=""
try() {
    label="$1"; cand="$2"
    if ! p="$(resolve "$cand")"; then
        # 目录在却没找到 —— 这个必须说, 不然用户对着一句"没找到"完全无从下手
        if [ -d "$cand" ]; then
            if has_gcc "$cand"; then
                MISSED="${MISSED}  - $label: $cand (gcc 在, 但起不来 —— 多半是可执行位丢了)
"
            else
                MISSED="${MISSED}  - $label: $cand (目录在, 但里面没有 $NAMES 的 gcc)
"
            fi
        fi
        return 0
    fi

    if [ -n "$TB" ]; then
        printf '#include <math.h>\nint main(void){return (int)floor(1.5);}\n' > "$TB/p.c" 2>/dev/null
        if ! "${p}gcc" -march=rv32imfc -mabi=ilp32f "$TB/p.c" -lm -o "$TB/p.elf" >"$TB/log" 2>&1; then
            SKIPPED="${SKIPPED}  - $label: $cand
      (编不过 rv32imfc/ilp32f, 不是 BL602 能用的那份)
"
            return 0
        fi
    fi

    echo "$p"
    exit 0
}

# ── 1/2) 明确指定的, 原样用 ───────────────────────────────
if p="$(resolve "${CONFIG_TOOLPREFIX:-}")"; then echo "$p"; exit 0; fi
if p="$(resolve "${BL60X_TOOLCHAIN_PATH:-}")"; then echo "$p"; exit 0; fi

# ── 3) PATH 里就有 ────────────────────────────────────────
if command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
    try "PATH" "$(dirname "$(command -v riscv64-unknown-elf-gcc)")"
fi

# ── 4) 常见的几种装法 ─────────────────────────────────────
for d in /opt/riscv /opt/riscv64-unknown-elf /opt/riscv-toolchain \
         "$HOME/riscv" "$HOME/opt/riscv" "$HOME/.local/riscv" /usr/local/riscv; do
    try "常见目录" "$d"
done

# ── 5) 仓库自带的 submodule (要先 git submodule update --init 才有) ──
SDK="${BL60X_SDK_PATH:-$(cd "$(dirname "$0")/.." && pwd)/sdk}"
try "仓库自带" "$SDK/toolchain/riscv/$PLATFORM"

{
    echo "find_toolchain: 没找到能用的 riscv64-unknown-elf 工具链"
    echo "  已查 \$CONFIG_TOOLPREFIX、\$BL60X_TOOLCHAIN_PATH、PATH、/opt/riscv 等常见目录、$SDK/toolchain/riscv/$PLATFORM"
    if [ -n "$MISSED" ]; then
        echo "  目录在、但没有工具链:"
        printf '%s' "$MISSED"
    fi
    if [ -n "$SKIPPED" ]; then
        echo "  找到了但不合适(跳过了):"
        printf '%s' "$SKIPPED"
    fi
} >&2
exit 1

#!/bin/bash
# 切到脚本所在目录，这样从任何位置运行都生效
cd "$(dirname "$0")"

SRC="/mnt/c/Users/lq950/Desktop/TEST3"
DST="WB2-clock2"

# 别人机器上没有 GUI Guider 项目时直接跳过（generated/ 已含最新 UI 代码）
if [ ! -d "$SRC" ]; then
    echo "⚠ 未找到 GUI Guider 项目: $SRC"
    echo "  跳过 GUI 同步（generated/ 已包含最新 UI 代码）"
    exit 0
fi

# ── Generated code ──
cp "$SRC/generated/screens/gg_screen.c"         "$DST/generated/screens/"
cp "$SRC/generated/screens/gg_layer_sys.c"      "$DST/generated/screens/"
cp "$SRC/generated/screens/gg_layer_top.c"      "$DST/generated/screens/"
cp "$SRC/generated/screens/gg_layer_bottom.c"   "$DST/generated/screens/"
cp "$SRC/generated/gui_guider.h"                "$DST/generated/"
cp "$SRC/generated/gg_utils.c"                  "$DST/generated/"
cp "$SRC/generated/gg_utils.h"                  "$DST/generated/"

# ── Events（TEST3 里是空 stub，保证与新 UI 一致）──
cp "$SRC/generated/events/gg_event.h"                 "$DST/generated/events/"
cp "$SRC/generated/events/gg_event_screen.c"          "$DST/generated/events/"
cp "$SRC/generated/events/gg_event_layer_sys.c"       "$DST/generated/events/"
cp "$SRC/generated/events/gg_event_layer_top.c"       "$DST/generated/events/"
cp "$SRC/generated/events/gg_event_layer_bottom.c"    "$DST/generated/events/"

# ── Image header（TEST3 新 UI 无图片，gg_image.h 为空）──
cp "$SRC/generated/assets/images/gg_image.h" "$DST/generated/assets/images/"

# ── Fonts header（只同步声明头；字体 .c 用工程内 v8 转换版）──
cp "$SRC/generated/assets/fonts/gg_font.h" "$DST/generated/assets/fonts/"

# ── Custom code ──
# 注意：custom/ 是手工维护的用户代码，不随 GUI Guider 同步！
# 曾经从这里覆盖，导致手工修复（image_2、wx_full_name、RGB565 图片）被旧版顶掉
#
# 字体：TEST3 导出的 v9 字体 .c 不拷贝（8bpp 2.1MB 且 v8 不兼容）。
# Morganite 的 v8 转换版由工程内 custom/lv_font_Morganite_Black_ttf_180.c 提供，
# gg_screen.c 里的引用名 lv_font_Morganite_Black_ttf_180 保持不变。
# ⚠ 重新生成时必须加 --no-compress --no-prefilter：
#   SDK LVGL8.3 的 LV_USE_FONT_COMPRESSED=0，压缩字体位图会渲染成空白（踩过坑）。
#   生成命令（符号名与 size/bpp 不得改）：
#   lv_font_conv --format lvgl --lv-include lvgl.h \
#     --font ../../桌面/Morganite-Black.ttf(已修复版) -r 0x30-0x3a \
#     --size 180 --bpp 4 --no-compress --no-prefilter --no-kerning \
#     --lv-font-name lv_font_Morganite_Black_ttf_180 --lv-fallback lv_font_montserrat_14 \
#     -o WB2-clock2/custom/lv_font_Morganite_Black_ttf_180.c

# ═══════════════════════════════════════════
# Fix LVGL 9 → 8 API
# ═══════════════════════════════════════════

# screen / scr
sed -i 's/lv_screen_load_anim_t/lv_scr_load_anim_t/g' "$DST/generated/gui_guider.h"
sed -i 's/lv_screen_load_anim(/lv_scr_load_anim(/g'   "$DST/generated/gg_utils.c"
sed -i 's/lv_screen_load(/lv_scr_load(/g'              "$DST/generated/gg_utils.c"
sed -i 's/lv_screen_active(/lv_scr_act(/g'             "$DST/generated/gg_utils.c"

# anim pause/resume: v9-only → v8 no-op (gg_event.h 动画控制辅助函数)
sed -i 's/lv_anim_pause(anim);/(void)anim; \/* v8: no anim_pause *\//' "$DST/generated/events/gg_event.h"
sed -i 's/lv_anim_resume(anim);/(void)anim; \/* v8: no anim_resume *\//' "$DST/generated/events/gg_event.h"

# layer_bottom: v9 的 lv_layer_bottom() 在 SDK LVGL 8.3 不存在 → 用 lv_scr_act()（同 WB2-clock 修法）
sed -i 's/lv_layer_bottom()/lv_scr_act()/' "$DST/generated/screens/gg_layer_bottom.c"

# gg_font.h: TEST3 导出的符号名（无 _ttf_）与 gg_screen.c 引用（带 _ttf_）不一致，
# 工程内 v8 字体符号名是 lv_font_Morganite_Black_ttf_180 → 统一声明头
sed -i 's/lv_font_Morganite_Black_180/lv_font_Morganite_Black_ttf_180/g' "$DST/generated/assets/fonts/gg_font.h"

echo "✔ GUI 代码同步完成"

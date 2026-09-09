/*
* Copyright 2026 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/


/*********************
 *      INCLUDES
 *********************/
#include <stdio.h>
#include "lvgl.h"
#include "custom.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**
 * Create a demo application
 */

void custom_init(gg_ui_t *ui)
{
    /* 温度/湿度图标回归 GUI-Guider 原图(gg_screen.c 默认 src), 不再覆盖为取反副本 */

    /* [临时注释] 天气图标背景(65x65 圆角灰渐变底衬) — 看效果用, 需要时取消注释恢复
    lv_obj_t *bg = lv_obj_create(ui->screen.screen);
    lv_obj_set_size(bg, 65, 65);
    lv_obj_set_pos(bg, 1, 108);
    lv_obj_set_style_radius(bg, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x909090), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(bg, lv_color_hex(0xE0E0E0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(bg, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(bg, LV_OPA_60, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bg, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_background(bg);
    */
}

/* 注: 新 UI(TEST3)只有 hour/minute/point, 无天气控件,
 * custom_set_wendu/custom_set_shidu 已随旧 UI 移除 */

const char *wx_full_name(const char *s)
{
    if(!s) return "?";
    if(strstr(s,"雷")) return "雷雨";
    if(strstr(s,"雨")) return "有雨";
    if(strstr(s,"雪")) return "有雪";
    if(strstr(s,"阴")) return "阴";
    if(strstr(s,"晴")) return "晴";
    if(strstr(s,"雾")) return "有雾";
    if(strstr(s,"沙")||strstr(s,"尘")) return "扬沙";
    return s;
}


#ifndef WX_ICONS_H
#define WX_ICONS_H
#include "lvgl.h"
extern const lv_font_t TEST;
extern const lv_font_t lv_font_cn24;   /* 24px 中文字库 (标题) */
extern const lv_font_t lv_font_cn18;   /* 18px 中文字库 (状态/AP名) */
extern const lv_img_dsc_t sunny_64x64_RGB565;
extern const lv_img_dsc_t cloudy_64x64_RGB565;
extern const lv_img_dsc_t light_rain_64x64_RGB565;
extern const lv_img_dsc_t heavy_rain_64x64_RGB565;

static const lv_img_dsc_t *wx_get_icon(const char *cn)
{
    if(strstr(cn,"晴")) return &sunny_64x64_RGB565;
    if(strstr(cn,"多云")||strstr(cn,"阴")||strstr(cn,"雪")||strstr(cn,"沙")||strstr(cn,"尘")) return &cloudy_64x64_RGB565;
    if(strstr(cn,"小雨")||strstr(cn,"阵雨")||strstr(cn,"雷")) return &light_rain_64x64_RGB565;
    if(strstr(cn,"中雨")||strstr(cn,"大雨")||strstr(cn,"暴雨")) return &heavy_rain_64x64_RGB565;
    return &sunny_64x64_RGB565;
}
#endif

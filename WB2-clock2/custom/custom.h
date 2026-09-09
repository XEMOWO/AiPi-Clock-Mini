/*
* Copyright 2026 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/

#ifndef CUSTOM_H
#define CUSTOM_H
#ifdef __cplusplus
extern "C" {
#endif

#include "../generated/gui_guider.h"

void custom_init(gg_ui_t *ui);

/** 天气轮询任务(双源: 高德 HTTP / 和风 HTTPS+gzip), 见 weather.c */
void weather_task(void *pv);

/** Map short weather name to full name (阴→阴天, 晴→晴天, etc.) */
const char *wx_full_name(const char *short_name);

/** 城市名 → 高德 adcode 候选列表("名称:代码;..."), 见 weather.c.
 * 返回静态缓冲指针, *n=候选数(≤maxn); 查询失败返回 NULL。 */
const char *wx_city_search(const char *name, int *n, int maxn);

#ifdef __cplusplus
}
#endif
#endif /* CUSTOM_H */

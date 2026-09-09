/**
 * @brief provision — 配网模式 (5× EN 复位触发, 软AP + captive portal + HTTP 配网页)
 *
 * 机制:
 *   - 开机计数存媒体分区独立扇区 (bootcnt.c), 由 SDK bfl_main 在串口
 *     初始化后、"Booting" 横幅前调用 app_boot_early 完成 —— 上电第一时间
 *     计数, 连续快速按 EN 5 次 → 进入配网模式
 *   - 配网模式: 软AP "Clock-Mini-XXXX" + DNS 重定向 + HTTP 配网页
 *   - 配网成功 → 写 easyflash 配置 → POR 复位 (同时清计数) → 正常模式
 */
#ifndef PROVISION_H
#define PROVISION_H

#include "version.h"   /* 产品名/版本号 (配网页 v{{VER}} 自带 v 前缀) */

#define PROV_NEED_COUNT 5   /* 累计开机次数阈值: >=5 → 进入配网模式 (main.c 出厂重置也用) */

int prov_boot_check(void);      /* main 上电第一时间调用: 计数/保持判断, 返回 1=配网模式 */
int prov_is_active(void);       /* 当前是否配网模式 */
void prov_keepalive_start(void);/* 正常模式: 稳定运行 3 分钟后清零开机计数 */
void prov_ui_init(void);        /* 创建配网界面并切换到配网屏 (LVGL 就绪后调用) */
void prov_start(void *arg);     /* 配网模式 WiFi 入口: 注册事件 + 启动固件任务 (替代 wifi_entry) */
void prov_enter(void);          /* 运行时进入配网(正常模式超时无网时调用): 停 STA + 切 UI + 启 AP */

#endif /* PROVISION_H */

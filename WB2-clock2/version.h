/**
 * @brief version — 产品信息统一来源 (main.c / xcmd.c / provision.c 引用)
 *        改产品名或版本号只改这一个文件。
 */
#ifndef VERSION_H
#define VERSION_H

#define PRODUCT_NAME "AiPi-Clock-Mini"   /* 产品名: 上电横幅 / #XVER / 软AP / 配网页 */
#define FW_VER_STR "0.0.2"               /* 发版版本号 (配网页 v{{VER}}、#XVER 显示) */
#define FW_BUILD_DATE __DATE__           /* 最后编译日期 */
#define FW_BUILD_TIME __TIME__           /* 最后编译时间 */

/* 出厂重置标记: 必须含编译时间戳 —— 每次重新编译都变化,
 * 保证新固件首次启动清一次旧配置; 不能改成固定版本号 */
#define FW_RESET_MARK "v" FW_VER_STR "-" __DATE__ "-" __TIME__

#endif /* VERSION_H */

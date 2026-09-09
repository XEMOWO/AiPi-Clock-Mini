/* 出厂内置开机图: 安信可 logo 白色黑底, 320x220 RGB565 小端 */
#ifndef BOOT_BUILTIN_HDR
#define BOOT_BUILTIN_HDR
#include <stdint.h>
#define BOOT_BUILTIN_W 320
#define BOOT_BUILTIN_H 220
#define BOOT_BUILTIN_BYTES (BOOT_BUILTIN_W * BOOT_BUILTIN_H * 2)
extern const uint8_t boot_builtin[BOOT_BUILTIN_BYTES];
#endif

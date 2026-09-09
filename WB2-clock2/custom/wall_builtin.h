/* 出厂内置壁纸: 安信可 logo 白色黑底, 320x190 RGB565 小端 */
#ifndef WALL_BUILTIN_HDR
#define WALL_BUILTIN_HDR
#include <stdint.h>
#define WALL_BUILTIN_W 320
#define WALL_BUILTIN_H 190
#define WALL_BUILTIN_BYTES (WALL_BUILTIN_W * WALL_BUILTIN_H * 2)
extern const uint8_t wall_builtin[WALL_BUILTIN_BYTES];
#endif

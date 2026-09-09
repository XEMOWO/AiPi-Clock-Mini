/**
 * @brief img_upload — 串口上传图片 → 写 media 分区 flash(纯存储管道, 不碰 LVGL)
 *
 * 协议(见 xcmd.c):
 *   #XIMGSTART,<hex(总字节数)>[,<hex(type)>]   开始: 按 type 擦除对应区域
 *   #XIMG,<hex(seq)>,<hex(len)>,<hex(data)>,<hex(xor)>  数据块(len ≤ 1000B)
 *   #XIMGEND                            结束: 校验总量并关闭, 之后由 main.c 切换显示
 *
 * media 分区布局 (290816B):
 *   [0, IMG_BOOT_BYTES)            开机图 320x220 RGB565 (上电显示 2 秒)
 *   [IMG_BOOT_BYTES, ...)          壁纸 320x190 RGB565 (图播放)
 */
#ifndef IMG_UPLOAD_H
#define IMG_UPLOAD_H

#include <stdint.h>

#define IMG_WALL_W 320   /* 可视区宽: 面板 172x320 竖屏横用, 逻辑列 0-319 全有效 */
#define IMG_WALL_H 190   /* 可视区高: OY=30 起点对齐, 可见 190 行 (0-189) */
#define IMG_WALL_BYTES (IMG_WALL_W * IMG_WALL_H * 2)  /* RGB565 */

#define IMG_BOOT_W 320   /* 开机图: 全屏 320x220 RGB565, 上电显示 2 秒 */
#define IMG_BOOT_H 220
#define IMG_BOOT_BYTES (IMG_BOOT_W * IMG_BOOT_H * 2)

#define IMG_TYPE_WALL 0   /* 壁纸 */
#define IMG_TYPE_BOOT 1   /* 开机图 */

/* 开始一次上传: 0=ok -1=分区打开失败 -2=超区域容量 */
int img_upload_start(uint32_t total, int type);

/* 写入一块(按 seq 定位, 重发幂等): 0=ok -1=未开始 -2=超总量 -3=flash 写失败 -4=乱序 */
int img_upload_chunk(uint16_t seq, uint16_t len, const uint8_t *data);

/* 结束上传(总量校验): 0=ok(可显示) -1=未开始或字节数不对 */
int img_upload_finish(void);

/* 诊断: 输出当前已确认字节数/期望总数(finish 失败时调用定位原因) */
void img_upload_stats(unsigned int *recv, unsigned int *total);

/* 壁纸显示(XIP 映射, 零 RAM): show=显示 media 分区图片, hide=擦除期间隐藏,
 * bind=开机绑定图源但不改变显隐(默认时钟, 轮播切换时才有图可显) */
void img_wall_show(void);
void img_wall_hide(void);
void img_wall_bind(void);

/* 图播放切换: show=只显示壁纸(隐藏其余 UI), hide=只显示时钟 UI。
 * 注意: 调用方必须已持有 lvgl_mutex(主循环持锁状态调用) */
void img_wall_set_vis(int show);

/* main.c 创建壁纸对象(全屏最底层, 默认隐藏) */
void img_wall_attach(lv_obj_t *parent);

/* 开机图 XIP 地址(0=ok 且 *xip 非空; -1=media 无 XIP 映射) */
int img_boot_get(const void **xip, uint32_t *size);

/* 过渡动画进行中(非 0)? 主循环据此冻结数字翻页等 lv_anim,
 * 避免隐藏中的对象被动画折腾, 也给动画帧腾出渲染时间 */
int img_wall_anim_busy(void);

#endif /* IMG_UPLOAD_H */

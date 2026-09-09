/**
 * @brief img_upload — 串口上传图片 → media 分区 flash(实现)
 *
 * 用 bl_mtd 按分区名打开 "media"(0x1A2000, 290KB); 分区布局:
 *   [0, IMG_BOOT_BYTES) = 开机图, [IMG_BOOT_BYTES, ...) = 壁纸
 * 各自按区域擦除/写入, 互不干扰; 显示用 XIP 映射地址直接画, 全程不占 heap。
 */
#include <stdio.h>
#include <string.h>
#include <lvgl.h>
#include <FreeRTOS.h>
#include <semphr.h>
#include "img_upload.h"
#include "cfg_store.h"          /* g_cfg.wall_anim: 切换过渡动画预设 */
#include "wall_builtin.h"       /* 出厂内置壁纸(未上传时的回退图源) */
#include <bl_mtd.h>

extern SemaphoreHandle_t lvgl_mutex;   /* main.c 定义, 与 weather.c 共用 */

static bl_mtd_handle_t s_h = NULL;   /* 打开后保持到 finish */
static unsigned int s_total = 0;     /* 期望总字节 */
static unsigned int s_recv = 0;      /* 已确认连续字节 */
static uint16_t s_expect_seq = 0;    /* 期望的下一个连续 seq */
static unsigned int s_base = 0;      /* 本次写入的起始偏移 (壁纸在开机图之后) */

int img_upload_start(uint32_t total, int type)
{
    bl_mtd_handle_t h = NULL;
    unsigned int sz = 0;
    unsigned int cap;

    if (s_h) { bl_mtd_close(s_h); s_h = NULL; }
    if (bl_mtd_open("media", &h, BL_MTD_OPEN_FLAG_NONE) != 0) return -1;
    bl_mtd_size(h, &sz);
    s_base = (type == IMG_TYPE_BOOT) ? 0 : IMG_BOOT_BYTES;
    cap = sz - s_base;                      /* 各自区域容量 */
    if (total > cap) { bl_mtd_close(h); return -2; }
    /* 只擦自己区域: 开机图与壁纸共存, 不能整区擦除 */
    if (bl_mtd_erase(h, s_base, ((total + 4095) / 4096) * 4096) != 0) {
        bl_mtd_close(h);
        return -3;
    }
    s_h = h;
    s_total = total;
    s_recv = 0;
    s_expect_seq = 0;
    return 0;
}

int img_upload_chunk(uint16_t seq, uint16_t len, const uint8_t *data)
{
    uint32_t off;

    if (!s_h) return -1;
    if (seq > s_expect_seq) return -4;  /* 乱序/缺块: 拒绝, 防止偏移错位后才在 finish 暴露 */
    off = s_base + (uint32_t)seq * len; /* 按 seq 定位(所有块等长时正确), 重发同 seq 幂等覆盖写 */
    if (off - s_base + len > s_total) return -2;
    if (bl_mtd_write(s_h, off, len, data) != 0) return -3;
    if (seq == s_expect_seq) {          /* 仅连续 seq 推进进度, 重发不计数 */
        s_expect_seq++;
        s_recv = (uint32_t)s_expect_seq * len;
    }
    return 0;
}

/* 开机图 XIP 地址: media 分区映射基址即开机图数据(偏移 0) */
int img_boot_get(const void **xip, uint32_t *size)
{
    bl_mtd_handle_t h = NULL;
    bl_mtd_info_t info;

    if (bl_mtd_open("media", &h, BL_MTD_OPEN_FLAG_BUSADDR) != 0) return -1;
    bl_mtd_info(h, &info);
    bl_mtd_close(h);
    if (!info.xip_addr) return -1;
    if (xip) *xip = info.xip_addr;
    if (size) *size = IMG_BOOT_BYTES;
    return 0;
}

void img_upload_stats(unsigned int *recv, unsigned int *total)
{
    if (recv) *recv = s_recv;
    if (total) *total = s_total;
}

int img_upload_finish(void)
{
    if (!s_h) {
        printf("[IMG] finish: no session\r\n");
        return -1;
    }
    if (s_recv != s_total) {
        printf("[IMG] finish fail: recv=%u total=%u\r\n", s_recv, s_total);
        return -1;
    }
    bl_mtd_close(s_h);
    s_h = NULL;
    return 0;
}

/* ===== 壁纸显示: media 分区经 XIP 映射为图片源, 全程零 RAM 拷贝 ===== */

static lv_obj_t *s_wall_img = NULL;

/* 前置声明: 动画区函数(img_wall_show 需要同步管理 UI 显隐) */
static void wall_snapshot(void);
static void wall_set_vis_final(int show);

/* 绑定图源(锁内): 0=ok -1=无壁纸对象。
 * 优先 media 分区 XIP(用户串口上传的图); 未上传过(wall_valid=0)时
 * 回退到固件内置壁纸(custom/wall_builtin.c), 出厂即带背景图。 */
static int wall_bind_src(void)
{
    bl_mtd_handle_t h = NULL;
    bl_mtd_info_t info;
    static lv_img_dsc_t dsc;      /* 内容每次刷新, data 指向 flash XIP 地址 */

    if (!s_wall_img) return -1;
    dsc.header.always_zero = 0;
    dsc.header.w = IMG_WALL_W;
    dsc.header.h = IMG_WALL_H;
    dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc.data_size = IMG_WALL_BYTES;
    if (g_cfg.wall_valid) {
        /* 关键: 必须带 BL_MTD_OPEN_FLAG_BUSADDR, 否则 xip_addr 恒为 0,
         * 用 NONE 打开会静默失败(壁纸对象无图源) */
        if (bl_mtd_open("media", &h, BL_MTD_OPEN_FLAG_BUSADDR) != 0) return -1;
        bl_mtd_info(h, &info);
        bl_mtd_close(h);
        if (!info.xip_addr) {
            printf("[WALL] no xip_addr (media bus addr fail)\r\n");
            return -1;
        }
        printf("[WALL] show: xip=%p off=0x%x sz=%u\r\n",
               info.xip_addr, (unsigned int)info.offset, (unsigned int)info.size);
        dsc.data = (const uint8_t *)info.xip_addr + IMG_BOOT_BYTES;   /* 壁纸在开机图之后 */
    } else {
        printf("[WALL] builtin wallpaper\r\n");
        dsc.data = wall_builtin;   /* 出厂内置壁纸(固件 flash, 只读) */
    }
    xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
    lv_img_set_src(s_wall_img, &dsc);
    xSemaphoreGive(lvgl_mutex);
    return 0;
}

/* 直显壁纸(上传完成/开机直显): 绑定图源 + 隐藏时钟 UI */
void img_wall_show(void)
{
    if (wall_bind_src() != 0) return;
    xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
    /* 直显壁纸时必须同步隐藏时钟 UI(壁纸在最底层, 不隐藏会重叠);
     * 同时建快照, 之后的轮播切换按快照正确恢复 */
    wall_snapshot();
    wall_set_vis_final(1);
    xSemaphoreGive(lvgl_mutex);
}

/* 开机绑定图源但不改变显隐(默认显示时钟, 轮播切换时才有图可显) */
void img_wall_bind(void)
{
    wall_bind_src();
}

void img_wall_hide(void)
{
    xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
    if (s_wall_img) lv_obj_add_flag(s_wall_img, LV_OBJ_FLAG_HIDDEN);
    xSemaphoreGive(lvgl_mutex);
}

/* ===== 图播放: 时钟/壁纸整屏切换(带过渡动画) =====
 * 快照在首次调用时记录 screen 各子对象的初始隐藏状态,
 * 恢复时按快照还原, 不会误显示原本就隐藏的对象。
 *
 * 动画驱动: 不用 lv_anim(按时间插值, 低渲染帧率下 200ms 只有 2-3 帧,
 * 看起来就是瞬间跳变), 改用 LVGL 定时器逐帧推进 — 帧数固定, 时长
 * 自动跟随渲染节奏: 每渲染一帧动画动一步, 慢设备动画自然变长,
 * 任何帧率下都能看清连续位移。
 *   wall_anim: 0=无 1=淡入 2=左滑推入 3=右滑推入
 *              4=上滑推入 5=缩放淡入 6=每次随机挑 1-5
 * 淡入/缩放: UI 直接进终态隐藏, 壁纸做渐显(时钟 UI 是黑底, 交叉淡化
 * 黑→黑看不出变化); 滑动: 切壁纸 = 壁纸滑入(UI 隐藏); 切回时钟 = 壁纸
 * 滑出 + UI 静态淡入。动画主体始终是壁纸位图(blit 快), 两个方向帧率
 * 一致 — 若让时钟 UI 参与滑动, 每帧要重绘几十个对象(字体/图标), 帧率
 * 掉一半, 看起来就是"一点点蹦出来"。
 * 动画进行中再次切换 → 中止当前动画直接跳终态(轮播间隔 >=1s 不会触发)。 */

#define WALL_SNAP_MAX 48
static lv_obj_t *s_snap_objs[WALL_SNAP_MAX];
static uint8_t   s_snap_hid[WALL_SNAP_MAX];
static int       s_snap_n = -1;   /* -1 = 未快照 */

#define WALL_ANIM_NONE    0
#define WALL_ANIM_FADE    1
#define WALL_ANIM_SLIDE_L 2
#define WALL_ANIM_SLIDE_R 3
#define WALL_ANIM_SLIDE_U 4
#define WALL_ANIM_ZOOM    5
#define WALL_ANIM_RANDOM  6

static lv_timer_t *s_anim_timer = NULL;
static int  s_anim_kind = -1;     /* -1=无动画进行中 */
static int  s_anim_show = 1;      /* 本次动画目标: 1=切到壁纸 0=切回时钟 */
static int  s_anim_frames = 12;   /* 总帧数(每渲染一帧推进一次) */
static int  s_anim_frame = 0;     /* 当前帧 */
#define UI_FADE_FRAMES 4          /* 切回时钟: 壁纸滑出后 UI 静态淡入帧数 */

/* 动画进行中(主循环据此冻结数字翻页, 避免隐藏中的对象被 lv_anim 折腾) */
int img_wall_anim_busy(void)
{
    return s_anim_kind >= 0;
}
static int  s_ui_x0[WALL_SNAP_MAX];
static int  s_ui_y0[WALL_SNAP_MAX];

static void wall_snapshot(void)
{
    lv_obj_t *c;
    uint32_t i, n;

    if (s_snap_n >= 0) return;
    s_snap_n = 0;
    n = lv_obj_get_child_cnt(lv_scr_act());
    for (i = 0; i < n && s_snap_n < WALL_SNAP_MAX; i++) {
        c = lv_obj_get_child(lv_scr_act(), (int32_t)i);
        if (c != s_wall_img) {
            s_snap_objs[s_snap_n] = c;
            s_snap_hid[s_snap_n] =
                lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) ? 1 : 0;
            s_snap_n++;
        }
    }
}

/* 终态落定(动画结束或中止后调用): 不经过动画直接设置 hidden */
static void wall_set_vis_final(int show)
{
    int i;

    if (show) {                      /* 只显示壁纸: 隐藏全部 UI */
        for (i = 0; i < s_snap_n; i++)
            lv_obj_add_flag(s_snap_objs[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_wall_img, LV_OBJ_FLAG_HIDDEN);
    } else {                         /* 恢复时钟 UI */
        for (i = 0; i < s_snap_n; i++) {
            if (s_snap_hid[i])
                lv_obj_add_flag(s_snap_objs[i], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_clear_flag(s_snap_objs[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_wall_img, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 复位动画期属性(位置/透明度/缩放), 恢复初始状态 */
static void wall_anim_reset(void)
{
    int i;

    lv_obj_set_pos(s_wall_img, 0, 0);
    lv_obj_set_style_opa(s_wall_img, LV_OPA_COVER, 0);
    lv_img_set_zoom(s_wall_img, 256);              /* 256 = 100% */
    for (i = 0; i < s_snap_n; i++) {
        lv_obj_set_pos(s_snap_objs[i], s_ui_x0[i], s_ui_y0[i]);
        lv_obj_set_style_opa(s_snap_objs[i], LV_OPA_COVER, 0);
    }
}

/* 动画收尾: 删定时器 + 复位 + 落定终态 */
static void wall_anim_finish(lv_timer_t *t)
{
    if (t) { lv_timer_del(t); s_anim_timer = NULL; }
    wall_anim_reset();
    wall_set_vis_final(s_anim_show);
    s_anim_kind = -1;
}

/* 切回时钟的滑动动画: 壁纸滑出(共 s_anim_frames 帧)结束后,
 * 时钟 UI 静态淡入收尾(UI 不参与滑动, 每帧只重绘壁纸位图, 帧率高) */
static void wall_anim_ui_fade_in(lv_timer_t *t)
{
    int i, ff;

    if (s_anim_frame == s_anim_frames + 1) {   /* 淡入第一帧: 壁纸退场, UI 就位 */
        lv_obj_add_flag(s_wall_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_wall_img, 0, 0);
        for (i = 0; i < s_snap_n; i++) {
            lv_obj_clear_flag(s_snap_objs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_snap_objs[i], s_ui_x0[i], s_ui_y0[i]);
            lv_obj_set_style_opa(s_snap_objs[i], LV_OPA_TRANSP, 0);
        }
    }
    ff = s_anim_frame - s_anim_frames - 1;     /* 0..UI_FADE_FRAMES-1 */
    if (ff >= UI_FADE_FRAMES) { wall_anim_finish(t); return; }
    for (i = 0; i < s_snap_n; i++)
        lv_obj_set_style_opa(s_snap_objs[i],
                             LV_OPA_COVER * (ff + 1) / UI_FADE_FRAMES, 0);
}

/* 逐帧推进: 每 lv_timer_handler 一次(渲染慢则自动变慢), tf ∈ (0,1000] */
static void wall_anim_tick(lv_timer_t *t)
{
    int tf;

    if (s_anim_kind < 0) { lv_timer_del(t); s_anim_timer = NULL; return; }
    s_anim_frame++;
    if (s_anim_frame > s_anim_frames &&
        s_anim_kind >= WALL_ANIM_SLIDE_L && s_anim_kind <= WALL_ANIM_SLIDE_U &&
        !s_anim_show) {
        wall_anim_ui_fade_in(t);           /* 壁纸滑出后的 UI 淡入阶段 */
        return;
    }
    if (s_anim_frame >= s_anim_frames) {
        wall_anim_finish(t);
        return;
    }
    tf = s_anim_frame * 1000 / s_anim_frames;

    switch (s_anim_kind) {
    case WALL_ANIM_FADE:             /* 壁纸渐显/渐隐(UI 已隐藏) */
        if (s_anim_show)
            lv_obj_set_style_opa(s_wall_img, LV_OPA_COVER * tf / 1000, 0);
        else
            lv_obj_set_style_opa(s_wall_img,
                                 LV_OPA_COVER - LV_OPA_COVER * tf / 1000, 0);
        break;
    case WALL_ANIM_ZOOM:             /* 壁纸缩放+透明度(UI 已隐藏) */
        if (s_anim_show) {
            lv_img_set_zoom(s_wall_img, 128 + (128 * tf) / 1000);
            lv_obj_set_style_opa(s_wall_img, LV_OPA_COVER * tf / 1000, 0);
        } else {
            lv_img_set_zoom(s_wall_img, 256 - (128 * tf) / 1000);
            lv_obj_set_style_opa(s_wall_img,
                                 LV_OPA_COVER - LV_OPA_COVER * tf / 1000, 0);
        }
        break;
    case WALL_ANIM_SLIDE_L:          /* 推入方向: 新从右进, 旧向左出 */
    case WALL_ANIM_SLIDE_R: {        /* 推入方向: 新从左进, 旧向右出 */
        int d = (s_anim_kind == WALL_ANIM_SLIDE_L) ? 1 : -1;
        if (s_anim_show)             /* 新画面 = 壁纸(UI 已隐藏): 壁纸滑入 */
            lv_obj_set_pos(s_wall_img, d * (320 - (320 * tf) / 1000), 0);
        else                         /* 新画面 = 时钟: 壁纸滑出, 露出渐变背景后
                                      * UI 淡入收尾(动画主体仍是壁纸位图, 帧率一致) */
            lv_obj_set_pos(s_wall_img, -d * (320 * tf) / 1000, 0);
        break;
    }
    case WALL_ANIM_SLIDE_U: {        /* 推入方向: 新从下进, 旧向上出 */
        if (s_anim_show)             /* 新画面 = 壁纸(UI 已隐藏): 壁纸滑入 */
            lv_obj_set_pos(s_wall_img, 0, 190 - (190 * tf) / 1000);
        else                         /* 新画面 = 时钟: 壁纸向下滑出, 收尾淡入 UI */
            lv_obj_set_pos(s_wall_img, 0, (190 * tf) / 1000);
        break;
    }
    default:
        break;
    }
}

/* 中止进行中的动画(位置可能处于中间态, 必须复位后再跳终态) */
static void wall_anim_abort(void)
{
    if (s_anim_kind < 0) return;
    if (s_anim_timer) { lv_timer_del(s_anim_timer); s_anim_timer = NULL; }
    wall_anim_reset();
    wall_set_vis_final(s_anim_show);
    s_anim_kind = -1;
}

/* 带过渡动画的整屏切换; 注意: 调用方必须已持有 lvgl_mutex */
void img_wall_set_vis(int show)
{
    int kind, i;

    if (!s_wall_img) return;
    wall_snapshot();
    kind = g_cfg.wall_anim;
    if (kind == WALL_ANIM_RANDOM) kind = 1 + lv_rand(0, 4);   /* 随机挑 1-5 */
    if (kind == WALL_ANIM_NONE) {        /* 直接切换 */
        wall_anim_abort();               /* 若上一动画未结束先复位 */
        wall_set_vis_final(show);
        return;
    }
    if (s_anim_kind >= 0) {              /* 动画进行中: 中止并跳终态 */
        wall_anim_abort();
        wall_set_vis_final(show);        /* 覆盖为新目标 */
        return;
    }

    s_anim_kind = kind;
    s_anim_show = show;
    s_anim_frame = 0;
    lv_obj_clear_flag(s_wall_img, LV_OBJ_FLAG_HIDDEN);
    if (kind == WALL_ANIM_FADE || kind == WALL_ANIM_ZOOM) {
        /* 渐显式: UI 直接进终态隐藏, 壁纸透明度/缩放动画(show=渐显, hide=渐隐) */
        s_anim_frames = 10;
        for (i = 0; i < s_snap_n; i++)
            lv_obj_add_flag(s_snap_objs[i], LV_OBJ_FLAG_HIDDEN);
        if (show) {
            lv_obj_set_style_opa(s_wall_img, LV_OPA_TRANSP, 0);
            if (kind == WALL_ANIM_ZOOM) lv_img_set_zoom(s_wall_img, 128);
        } else {
            lv_obj_set_style_opa(s_wall_img, LV_OPA_COVER, 0);
            if (kind == WALL_ANIM_ZOOM) lv_img_set_zoom(s_wall_img, 256);
        }
    } else {
        /* 滑动式: 只渲染"新画面"移动, 旧画面直接隐藏(渲染量减半,
         * 壁纸/UI 同屏移动在低帧率下视觉差异小, 但帧率接近翻倍) */
        s_anim_frames = 12;
        if (show) {                  /* 新画面 = 壁纸: UI 隐藏, 壁纸从屏外滑入 */
            for (i = 0; i < s_snap_n; i++) {
                /* 必须刷新位置: reset 结束时按 x0/y0 恢复, 不刷的话
                 * 开机后第一次 show 动画结束时会把 UI 甩到 (0,0) */
                s_ui_x0[i] = lv_obj_get_x(s_snap_objs[i]);
                s_ui_y0[i] = lv_obj_get_y(s_snap_objs[i]);
                lv_obj_add_flag(s_snap_objs[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (kind == WALL_ANIM_SLIDE_L) lv_obj_set_pos(s_wall_img, 320, 0);
            else if (kind == WALL_ANIM_SLIDE_R) lv_obj_set_pos(s_wall_img, -320, 0);
            else if (kind == WALL_ANIM_SLIDE_U) lv_obj_set_pos(s_wall_img, 0, 190);
        } else {                     /* 新画面 = 时钟: UI 保持隐藏, 壁纸从原位滑出,
                                      * 结束后 UI 静态淡入 — 动画主体始终是壁纸
                                      * 位图(blit 快), 两个方向帧率一致 */
            for (i = 0; i < s_snap_n; i++) {
                s_ui_x0[i] = lv_obj_get_x(s_snap_objs[i]);
                s_ui_y0[i] = lv_obj_get_y(s_snap_objs[i]);
                lv_obj_add_flag(s_snap_objs[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_set_pos(s_wall_img, 0, 0);   /* 壁纸从原位滑出 */
        }
    }
    s_anim_timer = lv_timer_create(wall_anim_tick, 1, NULL);
}

/* 由 main.c 创建壁纸对象(主流程串行执行, 无并发) */
void img_wall_attach(lv_obj_t *parent)
{
    s_wall_img = lv_img_create(parent);
    lv_obj_set_pos(s_wall_img, 0, 0);
    lv_obj_move_to_index(s_wall_img, 0);   /* LVGL8: index 0 = 最底层 */
    lv_obj_add_flag(s_wall_img, LV_OBJ_FLAG_HIDDEN);
}

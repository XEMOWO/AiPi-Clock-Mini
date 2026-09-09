/**
 * @brief xcmd — 串口命令协议实现
 *
 * RX 接入方式照抄 SDK CLI: aos_open("/dev/ttyS0") + aos_poll_read_fd
 * (参考 components/platform/soc/bl602/bl602/bfl_main.c:227)
 * 与 printf 日志共用 UART0 (2M bps),协议行以 "#X" 开头区分。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <aos/kernel.h>
#include <aos/yloop.h>
#include <vfs.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <wifi_mgmr_ext.h>
#include <sntp.h>
#include <utils_time.h>
#include <easyflash.h>
#include "xcmd.h"
#include "cfg_store.h"
#include "custom.h"     /* wx_city_search */
#include "img_upload.h" /* 图片上传 → media 分区 */
#include "version.h"    /* 产品名/版本号 (#XVER 响应) */

#define XCMD_LINE_MAX 2048   /* 图片块 ≤1000B(hex 2000 字符 + 帧头), 行缓冲需容纳 */
#define FW_VER PRODUCT_NAME " " FW_VER_STR   /* #XVER: "AiPi-Clock-Mini 1.0.0" */

extern SemaphoreHandle_t lvgl_mutex;   /* main.c 定义 */
extern int g_wall_vis;                 /* main.c 定义: 壁纸模式标志 */
extern void bg_apply(void);            /* main.c 定义: 应用时钟屏背景样式 */

static int fd_console = -1;
static uint8_t line_buf[XCMD_LINE_MAX];
static uint16_t line_len = 0;

static char s_ip_buf[16] = "-";
static int s_img_type = IMG_TYPE_WALL;   /* 本次图片上传类型 (IMGSTART 解析) */
/* 大块 hex 输出缓冲(候选列表等): 串口回调串行执行, 无并发; 栈只有 1KB
 * (event_loop task), 大缓冲不能放栈上 */
static char s_ack_hex[640];

/* ===== hex 编解码 ===== */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 解码 hex 串到 out,返回字节数;遇到非法字符停止 */
static int hex_decode(const char *hex, char *out, int max)
{
    int n = 0;

    while (n < max && hex[0] && hex[1]) {
        int hi = hex_val(hex[0]), lo = hex_val(hex[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (char)((hi << 4) | lo);
        hex += 2;
    }
    out[n] = 0;
    return n;
}

/* 编码字符串为小写 hex 到 out,返回输出长度(不含结尾 0) */
static int hex_encode(const char *s, char *out)
{
    int n = 0;

    while (*s) {
        out[n++] = "0123456789abcdef"[((uint8_t)*s >> 4) & 0x0f];
        out[n++] = "0123456789abcdef"[((uint8_t)*s) & 0x0f];
        s++;
    }
    out[n] = 0;
    return n;
}

/* ===== 响应输出 ===== */

static void ack(const char *res, const char *cmd, const char *data)
{
    if (data && data[0])
        printf("#XA,%s,%s,%s\r\n", res, cmd, data);
    else
        printf("#XA,%s,%s\r\n", res, cmd);
}

static void ack_err(const char *cmd, const char *msg)
{
    char h[128];

    hex_encode(msg, h);
    ack("ERR", cmd, h);
}

/* ===== 命令处理 ===== */

static void xcmd_wifi_reconnect(void)
{
    wifi_interface_t iface = wifi_mgmr_sta_enable();
    wifi_mgmr_sta_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    wifi_mgmr_sta_connect(&iface, g_cfg.ssid, g_cfg.pwd, NULL, NULL, 0, 0);
}

static void cmd_cfg(char **args, int nargs)
{
    char ssid[33] = {0}, pwd[65] = {0}, city[16] = {0};
    int sn, pn, cn;
    char h1[96], h2[64], data[176];

    if (nargs < 4) { ack_err("CFG", "no args"); return; }
    sn = hex_decode(args[1], ssid, sizeof(ssid) - 1);
    pn = hex_decode(args[2], pwd, sizeof(pwd) - 1);
    cn = hex_decode(args[3], city, sizeof(city) - 1);
    if (sn == 0 && pn == 0 && cn == 0) { ack_err("CFG", "no data"); return; }
    if (sn > 32) { ack_err("CFG", "ssid>32"); return; }
    if (pn > 64) { ack_err("CFG", "pwd>64"); return; }

    if (sn > 0) strcpy(g_cfg.ssid, ssid);
    if (pn > 0) strcpy(g_cfg.pwd, pwd);
    if (cn > 0) strcpy(g_cfg.city, city);
    if (cfg_save(&g_cfg) != 0) { ack_err("CFG", "ef save fail"); return; }

    hex_encode(g_cfg.ssid, h1);
    hex_encode(g_cfg.city, h2);
    snprintf(data, sizeof(data), "%s,%s", h1, h2);
    ack("OK", "CFG", data);

    g_cfg_dirty = 1;   /* weather_task 下一轮立即重查 */
    xcmd_wifi_reconnect();
}

/* 城市名 → 候选列表(高德 geocode 联网查询), 返回候选数;
 * -1=未设高德key -2=查询失败(网络/JSON/响应过大) */
static int city_search(const char *name, const char **out)
{
    int n = 0;

    *out = wx_city_search(name, &n, 5);
    if (!*out)
        return !g_cfg.amap_key[0] ? -1 : -2;
    return n;
}

/* 从 "名称:代码;名称:代码;" 中取第 idx 个代码到 out */
static int city_code_at(const char *list, int idx, char *out, int outcap)
{
    const char *p = list;
    int i = 0;

    while (*p && i <= idx) {
        const char *colon = strchr(p, ':');
        const char *semi = colon ? strchr(colon + 1, ';') : NULL;
        int cl;

        if (!colon || !semi) return -1;
        cl = (int)(semi - (colon + 1));
        if (i == idx) {
            if (cl <= 0 || cl >= outcap) return -1;
            memcpy(out, colon + 1, cl);
            out[cl] = 0;
            return 0;
        }
        p = semi + 1;
        i++;
    }
    return -1;
}

/* #XCITY,<hex(city)> — 直接设置城市名/代码(中文名原样保存, 各天气源自行解析:
 * Open-Meteo 按中文名反查, 高德/和风按 adcode 或中文名查询) */
static void cmd_city(char **args, int nargs)
{
    char city[16] = {0};
    int cn;
    char h[64];

    if (nargs < 2) { ack_err("CITY", "no args"); return; }
    cn = hex_decode(args[1], city, sizeof(city) - 1);
    if (cn == 0 || cn > 15) { ack_err("CITY", "bad city"); return; }
    strcpy(g_cfg.city, city);
    if (cfg_save(&g_cfg) != 0) { ack_err("CITY", "ef save fail"); return; }

    hex_encode(g_cfg.city, h);
    ack("OK", "CITY", h);
    g_cfg_dirty = 1;
}

/* #XWXCITY,<hex(城市名)> — 只查询, 返回候选列表(不设置) */
static void cmd_wxcity(char **args, int nargs)
{
    char name[48] = {0};
    const char *list;
    int nn;

    if (nargs < 2) { ack_err("WXCITY", "no args"); return; }
    nn = hex_decode(args[1], name, sizeof(name) - 1);
    if (nn == 0) { ack_err("WXCITY", "bad name"); return; }
    nn = city_search(name, &list);
    if (nn < 0) { ack_err("WXCITY", nn == -1 ? "need amap key" : "query fail"); return; }
    if (nn == 0) { ack_err("WXCITY", "no match"); return; }
    hex_encode(list, s_ack_hex);
    ack("OK", "WXCITY", s_ack_hex);
}

/* 天气源切换: #XWXAPI,<hex("amap"|"qweather"|"openmeteo")> */
static void cmd_wxapi(char **args, int nargs)
{
    char api[16] = {0};
    int n;
    char h[64];

    if (nargs < 2) { ack_err("WXAPI", "no args"); return; }
    n = hex_decode(args[1], api, sizeof(api) - 1);
    if (n == 0 || (strcmp(api, "amap") != 0 && strcmp(api, "qweather") != 0
                   && strcmp(api, "openmeteo") != 0)) {
        ack_err("WXAPI", "bad api (amap|qweather|openmeteo)"); return;
    }
    strcpy(g_cfg.wx_api, api);
    if (cfg_save(&g_cfg) != 0) { ack_err("WXAPI", "ef save fail"); return; }
    hex_encode(g_cfg.wx_api, h);
    ack("OK", "WXAPI", h);
    g_cfg_dirty = 1;
}

/* API Key 下发: #XWXKEY,<hex(provider)>,<hex(key)>[,<hex(cred)>]
 * provider=amap|qweather; qweather 第三段为旧版凭据ID(仅存储备用,认证用
 * X-QW-Api-Key 头) */
static void cmd_wxkey(char **args, int nargs)
{
    char prov[16] = {0}, key[33] = {0}, cred[33] = {0};
    int pn, kn, cren = 0;
    char h[64];

    if (nargs < 3) { ack_err("WXKEY", "no args"); return; }
    pn = hex_decode(args[1], prov, sizeof(prov) - 1);
    kn = hex_decode(args[2], key, sizeof(key) - 1);
    if (nargs > 3) cren = hex_decode(args[3], cred, sizeof(cred) - 1);
    if (pn == 0 || kn == 0 || kn > 32) { ack_err("WXKEY", "bad args"); return; }
    if (strcmp(prov, "amap") == 0) {
        strcpy(g_cfg.amap_key, key);
    } else if (strcmp(prov, "qweather") == 0) {
        strcpy(g_cfg.qw_key, key);
        if (cren > 0) strcpy(g_cfg.qw_cred, cred);
    } else {
        ack_err("WXKEY", "bad provider"); return;
    }
    if (cfg_save(&g_cfg) != 0) { ack_err("WXKEY", "ef save fail"); return; }
    hex_encode(key, h);
    ack("OK", "WXKEY", h);
    g_cfg_dirty = 1;
}

/* 时钟显示样式: #XCLOCK,<hex("1"=滚动翻页 | "0"=直接切换)> */
static void cmd_clock(char **args, int nargs)
{
    char v[2] = {0};
    int n;
    char h[8];

    if (nargs < 2) { ack_err("CLOCK", "no args"); return; }
    n = hex_decode(args[1], v, 1);
    if (n == 0 || (v[0] != '0' && v[0] != '1')) {
        ack_err("CLOCK", "bad anim (0|1)"); return;
    }
    g_cfg.clock_anim = v[0] - '0';
    if (cfg_save(&g_cfg) != 0) { ack_err("CLOCK", "ef save fail"); return; }
    hex_encode(v, h);
    ack("OK", "CLOCK", h);
}

/* #XIMGSTART,<hex(总字节数)>[,<hex(type)>] — 开始上传图片
 * type: 0=壁纸(默认, 向后兼容) 1=开机图; 按 type 只擦对应区域 */
static void cmd_imgstart(char **args, int nargs)
{
    char buf[8];
    uint32_t total = 0;
    int n, type = IMG_TYPE_WALL;

    if (nargs < 2) { ack_err("IMGSTART", "no args"); return; }
    n = hex_decode(args[1], buf, 4);          /* 最多 4 字节 = 32 位 */
    if (n == 0) { ack_err("IMGSTART", "bad total"); return; }
    for (int i = 0; i < n; i++)
        total = (total << 8) | (uint8_t)buf[i];
    if (nargs >= 3 && hex_decode(args[2], buf, 1) == 1)
        type = (uint8_t)buf[0];
    n = img_upload_start(total, type);
    if (n != 0) { ack_err("IMGSTART", n == -2 ? "too big" : "mtd fail"); return; }
    s_img_type = type;
    if (type == IMG_TYPE_WALL)
        img_wall_hide();        /* 擦除期间不再读旧图(XIP 会返回乱数据) */
    ack("OK", "IMGSTART", NULL);
}

/* #XIMG,<hex(seq)>,<hex(len)>,<hex(data)>,<hex(xor 校验和)> — 一块数据(len≤1000B)
 * XOR 校验: 2M 下丢字节致 hex 奇偶错位时, 解码长度可能恰好匹配但内容错误,
 * 无校验会静默写入损坏数据(工具端无法察觉); 校验失败回 ERR 触发重发 */
static void cmd_img(char **args, int nargs)
{
    static char data[1024];       /* 栈只有 1KB, 数据缓冲放静态区 */
    uint16_t seq, len;
    uint8_t chk = 0;
    int n;

    if (nargs < 5) { ack_err("IMG", "no args"); return; }
    n = hex_decode(args[1], (char *)&seq, 2);
    if (n != 2) { ack_err("IMG", "bad seq"); return; }
    n = hex_decode(args[2], (char *)&len, 2);
    if (n != 2 || len == 0 || len > 1000) { ack_err("IMG", "bad len"); return; }
    n = hex_decode(args[3], data, 1000);
    if (n != len) { ack_err("IMG", "bad hex"); return; }
    n = hex_decode(args[4], (char *)&chk, 1);
    if (n != 1) { ack_err("IMG", "bad chk"); return; }
    for (int i = 0; i < len; i++) chk ^= (uint8_t)data[i];
    if (chk != 0) { ack_err("IMG", "chk fail"); return; }
    n = img_upload_chunk(seq, len, (const uint8_t *)data);
    if (n != 0) { ack_err("IMG", n == -2 ? "over total" : "write fail"); return; }
    ack("OK", "IMG", NULL);
}

/* #XBOOTCLR — 清除开机图(移除图片数据标志, 上电不再显示) */
static void cmd_bootclr(char **args, int nargs)
{
    (void)args; (void)nargs;
    ef_set_and_save_env("w_bootimg", "0");
    ack("OK", "BOOTCLR", NULL);
}

/* #XBOOTIMG,<hex(0|1)> — 开机图显示开关 */
static void cmd_bootimg(char **args, int nargs)
{
    char buf[2];
    if (nargs < 2 || hex_decode(args[1], buf, 1) != 1) {
        ack_err("BOOTIMG", "no args");
        return;
    }
    ef_set_and_save_env("w_booton", buf[0] ? "1" : "0");
    ack("OK", "BOOTIMG", NULL);
}

/* #XIMGEND — 结束上传: 校验总量, 关闭分区; 壁纸切换显示, 开机图置有效标志 */
static void cmd_imgend(char **args, int nargs)
{
    if (img_upload_finish() != 0) {
        unsigned int rv = 0, tt = 0;
        char m[48];
        img_upload_stats(&rv, &tt);
        snprintf(m, sizeof(m), "recv %u/%u", rv, tt);
        ack_err("IMGEND", m);           /* 工具端会解码显示, 便于定位缺块/未开始 */
        return;
    }
    if (s_img_type == IMG_TYPE_BOOT) {
        ef_set_and_save_env("w_bootimg", "1");   /* 开机图有效: 上电显示 2 秒 */
        ef_set_and_save_env("w_booton", "1");    /* 上传即启用 */
        ack("OK", "IMGEND", NULL);
        return;
    }
    g_cfg.wall_valid = 1;         /* 壁纸已入库 */
    g_cfg.wall_mode = 1;          /* 自动开启图播放: 否则主循环 wall_mode=0 分支会立刻隐藏壁纸 */
    cfg_save(&g_cfg);
    /* 上传后的新图必须重新绑定 XIP 图源(img_wall_show 内部取锁):
     * 不绑定的话壁纸对象还是旧图源/空图源, 显示出来就是没有图片 */
    g_wall_vis = 1;               /* 通知主循环: 当前处于壁纸模式 */
    img_wall_show();              /* 绑定图源 + 隐藏时钟 UI + 直显壁纸 */
    ack("OK", "IMGEND", NULL);
}

/* 图播放: #XWALL,<hex(mode 0|1)>,<hex(壁纸秒数 1-60)>[,<hex(动画 0-6)>][,<hex(时钟秒数 1-60)>]
 * 可选参数不传 = 保持原设置, 向后兼容: 动画 0=无 1=淡入 2=左滑
 * 3=右滑 4=上滑 5=缩放淡入 6=随机; 时钟秒数 = 时钟页面显示时长 */
static void cmd_wall(char **args, int nargs)
{
    char m[2] = {0}, s[4] = {0};
    uint8_t sec = 0;
    int mn, sn;
    char h[8];

    if (nargs < 3) { ack_err("WALL", "no args"); return; }
    mn = hex_decode(args[1], m, 1);
    if (mn == 0 || (m[0] != '0' && m[0] != '1')) {
        ack_err("WALL", "bad mode (0|1)"); return;
    }
    sn = hex_decode(args[2], s, sizeof(s) - 1);
    sec = (uint8_t)strtol(s, NULL, 10);
    if (sn == 0 || sec < 1 || sec > 60) {
        ack_err("WALL", "bad sec (1-60)"); return;
    }
    g_cfg.wall_mode = m[0] - '0';
    g_cfg.wall_sec = sec;
    if (nargs >= 4) {
        char an[4] = {0};
        int anv;
        if (hex_decode(args[3], an, sizeof(an) - 1) == 0) {
            ack_err("WALL", "bad anim"); return;
        }
        anv = (int)strtol(an, NULL, 10);
        if (anv < 0 || anv > 6) {
            ack_err("WALL", "bad anim (0-6)"); return;
        }
        g_cfg.wall_anim = (uint8_t)anv;
    }
    if (nargs >= 5) {
        char cs[4] = {0};
        int csv;
        if (hex_decode(args[4], cs, sizeof(cs) - 1) == 0) {
            ack_err("WALL", "bad clock sec"); return;
        }
        csv = (int)strtol(cs, NULL, 10);
        if (csv < 1 || csv > 60) {
            ack_err("WALL", "bad clock sec (1-60)"); return;
        }
        g_cfg.wall_clock_sec = (uint8_t)csv;
    }
    if (cfg_save(&g_cfg) != 0) { ack_err("WALL", "ef save fail"); return; }
    hex_encode(m, h);
    ack("OK", "WALL", h);
}

/* 时钟屏背景: #XBG,<hex(0xRRGGBB 起色)>,<hex(0xRRGGBB 止色)>,<hex(样式)>
 * 样式: 0=纯色(用起色) 1=水平渐变 2=垂直渐变; 保存并立即应用 */
static void cmd_bg(char **args, int nargs)
{
    char h1[8] = {0}, h2[8] = {0}, d[2] = {0};
    uint32_t c1, c2;
    int dir;

    if (nargs < 4) { ack_err("BG", "no args"); return; }
    if (hex_decode(args[1], h1, sizeof(h1) - 1) == 0 ||
        hex_decode(args[2], h2, sizeof(h2) - 1) == 0 ||
        hex_decode(args[3], d, sizeof(d) - 1) == 0) {
        ack_err("BG", "bad hex"); return;
    }
    c1 = (uint32_t)strtoul(h1, NULL, 16);
    c2 = (uint32_t)strtoul(h2, NULL, 16);
    dir = (int)strtol(d, NULL, 10);
    if (c1 > 0xFFFFFF || c2 > 0xFFFFFF || dir < 0 || dir > 2) {
        ack_err("BG", "bad val"); return;
    }
    g_cfg.bg_c1 = c1;
    g_cfg.bg_c2 = c2;
    g_cfg.bg_dir = (uint8_t)dir;
    if (cfg_save(&g_cfg) != 0) { ack_err("BG", "ef save fail"); return; }
    xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
    bg_apply();
    xSemaphoreGive(lvgl_mutex);
    ack("OK", "BG", args[1]);
}

/* Claude Code 状态监控开关: #XMON,<0|1> */
static void cmd_mon(char **args, int nargs)
{
    if (nargs < 2) { ack_err("MON", "no args"); return; }
    g_cfg.mon_on = (args[1][0] == '1') ? 1 : 0;
    if (cfg_save(&g_cfg) != 0) { ack_err("MON", "ef save fail"); return; }
    ack("OK", "MON", g_cfg.mon_on ? "on" : "off");
}

/* Claude Code 状态灯: #XLAMP,<0-4> (0=呼吸 1/2=黄 3=绿 4=红) */
static void cmd_lamp(char **args, int nargs)
{
    if (nargs < 2 || args[1][0] < '0' || args[1][0] > '4' || args[1][1] != 0) {
        ack_err("LAMP", "bad value"); return;
    }
    g_lamp_status = (int8_t)(args[1][0] - '0');
    ack("OK", "LAMP", args[1]);
}

static const char *state_name(int st)
{
    switch (st) {
        case WIFI_STATE_IDLE:                 return "IDLE";
        case WIFI_STATE_CONNECTING:           return "CONNECTING";
        case WIFI_STATE_CONNECTED_IP_GETTING: return "CONNECTED_IP_GETTING";
        case WIFI_STATE_CONNECTED_IP_GOT:     return "CONNECTED_IP_GOT";
        case WIFI_STATE_DISCONNECT:           return "DISCONNECT";
        case WIFI_STATE_PSK_ERROR:            return "PSK_ERROR";
        case WIFI_STATE_NO_AP_FOUND:          return "NO_AP_FOUND";
        case WIFI_STATE_IFDOWN:               return "IFDOWN";
        default:                              return "UNKNOWN";
    }
}

static void cmd_sta(void)
{
    int st = WIFI_STATE_UNKNOWN;
    uint32_t a = 0, gw = 0, mask = 0;
    int rssi = -127;
    uint32_t sec = 0, frag = 0;
    char ip[16] = "-", tim[24] = "-";
    char sh[96], ch[64], wxh[48];

    wifi_mgmr_sta_state_get(&st);
    if (wifi_mgmr_sta_ip_get(&a, &gw, &mask) == 0 && a) {
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                 a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, (a >> 24) & 0xff);
    }
    wifi_mgmr_rssi_get(&rssi);
    sntp_get_time(&sec, &frag);
    if (sec > 1000000000) {
        utils_time_date_t d;
        utils_time_date_from_epoch(sec + 8 * 3600, &d);
        snprintf(tim, sizeof(tim), "%04d-%02d-%02d %02d:%02d:%02d",
                 d.ntp_year, d.ntp_month, d.ntp_date,
                 d.ntp_hour, d.ntp_minute, d.ntp_second);
    }
    hex_encode(g_cfg.ssid, sh);
    hex_encode(g_cfg.city, ch);
    hex_encode(g_cfg.wx_api, wxh);
    printf("#XST,%s,%s,%s,%d,%s,%s,%s\r\n",
           state_name(st), sh, ip, rssi, tim, ch, wxh);
}

/* ===== 行分发 ===== */

static void xcmd_dispatch(void)
{
    char *rest = (char *)line_buf + 2;
    char *args[5];
    int nargs = 0;
    char *p;

    line_buf[line_len] = '\0';   /* 确保字符串终止,防上次残留污染 */
    printf("[XCMD] rx(%d)\r\n", line_len);

    args[nargs++] = rest;
    for (p = rest; *p && nargs < 5; p++) {
        if (*p == ',') { *p = 0; args[nargs++] = p + 1; }
    }
    if (nargs < 1) return;

    if      (strcmp(args[0], "PING") == 0) ack("OK", "PING", NULL);
    else if (strcmp(args[0], "VER")  == 0) {
        char h[64];
        hex_encode(FW_VER, h);
        ack("OK", "VER", h);
    }
    else if (strcmp(args[0], "CFG")  == 0) cmd_cfg(args, nargs);
    else if (strcmp(args[0], "CITY") == 0) cmd_city(args, nargs);
    else if (strcmp(args[0], "STA")  == 0) cmd_sta();
    else if (strcmp(args[0], "MON")  == 0) cmd_mon(args, nargs);
    else if (strcmp(args[0], "LAMP") == 0) cmd_lamp(args, nargs);
    else if (strcmp(args[0], "CLOCK") == 0) cmd_clock(args, nargs);
    else if (strcmp(args[0], "IMGSTART") == 0) cmd_imgstart(args, nargs);
    else if (strcmp(args[0], "IMG") == 0) cmd_img(args, nargs);
    else if (strcmp(args[0], "IMGEND") == 0) cmd_imgend(args, nargs);
    else if (strcmp(args[0], "BOOTCLR") == 0) cmd_bootclr(args, nargs);
    else if (strcmp(args[0], "BOOTIMG") == 0) cmd_bootimg(args, nargs);
    else if (strcmp(args[0], "WALL") == 0) cmd_wall(args, nargs);
    else if (strcmp(args[0], "BG")   == 0) cmd_bg(args, nargs);
    else if (strcmp(args[0], "WXAPI") == 0) cmd_wxapi(args, nargs);
    else if (strcmp(args[0], "WXKEY") == 0) cmd_wxkey(args, nargs);
    else if (strcmp(args[0], "WXCITY") == 0) cmd_wxcity(args, nargs);
    else ack("ERR", "UNKNOWN", NULL);
}

/* 逐字节拼行,收到 \n 触发分发 */
static void xcmd_feed(const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        uint8_t c = data[i];

        if (c == '\n') {
            if (line_len >= 2 && line_buf[0] == '#' && line_buf[1] == 'X') {
                if (line_buf[line_len - 1] == '\r') line_len--;
                xcmd_dispatch();
            }
            line_len = 0;
        } else if (c == '\r') {
            /* 忽略,由 \n 触发 */
        } else if (c == 0) {
            /* 忽略 RX 噪声字节(CH340 TX 空闲低电平时会产生 \x00 流) */
        } else if (line_len < XCMD_LINE_MAX - 1) {
            line_buf[line_len++] = c;
        } else {
            /* 行溢出(粘帧超限): 整行作废。必须丢弃到 '\n' 为止,
             * 否则残留的半个粘帧会污染下一帧(下一帧被吞、ACK 超时) */
            printf("[XCMD] line overflow, dropped\r\n");
            line_len = 0;
            while (i < len && data[i] != '\n') i++;  /* '\n' 由外层 for 的 i++ 跳过 */
        }
    }
}

/* ===== UART RX 接入(yloop 回调,同 CLI 模式) ===== */

static void xcmd_uart_cb(int fd, void *param)
{
    static char tmp[256];   /* 静态: 避免压榨 yloop 任务栈; 回调串行执行无并发 */
    int n;

    /* 循环读到 RX 流缓冲空(非阻塞, 空时立即返回 0):
       整帧必须一次回调收完整, 否则残余字节留到下次回调(可能不再触发) */
    while ((n = aos_read(fd, tmp, sizeof(tmp))) > 0) {
        xcmd_feed((uint8_t *)tmp, n);
    }
}

void xcmd_init(void)
{
    fd_console = aos_open("/dev/ttyS0", 0);
    if (fd_console >= 0)
        aos_poll_read_fd(fd_console, xcmd_uart_cb, NULL);
    else
        printf("[XCMD] open /dev/ttyS0 failed\r\n");
}

/* ===== 对外接口 ===== */

void xcmd_send_event(const char *ev, const char *data)
{
    if (data && data[0])
        printf("#XEV,%s,%s\r\n", ev, data);
    else
        printf("#XEV,%s\r\n", ev);
}

const char *xcmd_ip_str(void)
{
    uint32_t a = 0, gw = 0, mask = 0;

    if (wifi_mgmr_sta_ip_get(&a, &gw, &mask) == 0 && a) {
        snprintf(s_ip_buf, sizeof(s_ip_buf), "%u.%u.%u.%u",
                 a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, (a >> 24) & 0xff);
    } else {
        strcpy(s_ip_buf, "-");
    }
    return s_ip_buf;
}

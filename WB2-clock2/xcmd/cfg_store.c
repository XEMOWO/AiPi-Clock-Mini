/**
 * @brief cfg_store — easyflash 持久化封装实现
 *        注意: easyflash_init() 已由 SDK 在 aos_loop_proc 自动调用,无需应用初始化
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <easyflash.h>
#include "cfg_store.h"

#define K_SSID "w_ssid"
#define K_PWD  "w_pwd"
#define K_CITY "w_city"
#define K_PCIP "w_pc_ip"
#define K_MON  "w_mon"
#define K_WXON "w_wxon"   /* 天气开关: 默认关 */
#define K_WXAPI "w_wxapi"
#define K_AMAPK "w_amapk"
#define K_QWKEY "w_qwkey"
#define K_QWCRED "w_qwcred"
#define K_CLK    "w_clk"
#define K_WALL   "w_wall"
#define K_WMODE  "w_wmode"
#define K_WSEC   "w_wsec"
#define K_WCLK   "w_wclk"
#define K_WANIM  "w_wanim"
#define K_BGC1   "w_bgc1"
#define K_BGC2   "w_bgc2"
#define K_BGDIR  "w_bgdir"

#define DEFAULT_SSID ""   /* 出厂无 WiFi 配置: 首次开机直接进配网 */
#define DEFAULT_PWD  ""
#define DEFAULT_CITY "440306"
#define DEFAULT_WXAPI  "amap"
#define DEFAULT_AMAPK  "c01d70381da92dee9c4f16320555d685"
#define DEFAULT_QWKEY  "61622d11bffc476aa10b36e08c0f0f4a"
#define DEFAULT_QWCRED "HE2301271524131032"
#define DEFAULT_CLOCK_ANIM 1   /* 默认滚动翻页 */
#define DEFAULT_WALL_MODE 0    /* 图播放默认关 */
#define DEFAULT_WALL_SEC  5    /* 壁纸默认显示 5 秒 */
#define DEFAULT_WALL_CLOCK_SEC 5  /* 时钟默认显示 5 秒 */
#define DEFAULT_WALL_ANIM 0    /* 过渡动画默认无(直接切换) */
#define DEFAULT_BG_C1 0xCDE6F8 /* 背景渐变起色: 淡蓝 */
#define DEFAULT_BG_C2 0xE9DCF6 /* 背景渐变止色: 淡紫 */
#define DEFAULT_BG_DIR 2       /* 背景样式默认: 垂直渐变 */

cfg_t g_cfg;
volatile uint8_t g_cfg_dirty = 0;
volatile int8_t g_lamp_status = -1;

void cfg_load_default(cfg_t *c)
{
    strcpy(c->ssid, DEFAULT_SSID);
    strcpy(c->pwd,  DEFAULT_PWD);
    strcpy(c->city, DEFAULT_CITY);
    strcpy(c->wx_api,   DEFAULT_WXAPI);
    strcpy(c->amap_key, DEFAULT_AMAPK);
    strcpy(c->qw_key,   DEFAULT_QWKEY);
    strcpy(c->qw_cred,  DEFAULT_QWCRED);
    c->wx_on = 0;   /* 天气默认关闭(代码保留, 需要时改配置或命令开启) */
    c->clock_anim = DEFAULT_CLOCK_ANIM;
    c->wall_mode = DEFAULT_WALL_MODE;
    c->wall_sec  = DEFAULT_WALL_SEC;
    c->wall_clock_sec = DEFAULT_WALL_CLOCK_SEC;
    c->wall_anim = DEFAULT_WALL_ANIM;
    c->bg_c1 = DEFAULT_BG_C1;
    c->bg_c2 = DEFAULT_BG_C2;
    c->bg_dir = DEFAULT_BG_DIR;
}

int cfg_load(cfg_t *c)
{
    const char *v;
    size_t blen = 0;

    cfg_load_default(c);
    /* ssid/pwd/city 可能含中文(UTF-8), ef_get_env 会把非 ASCII 判定为非字符串
     * 返回 NULL, 必须用 blob 读取 */
    if (ef_get_env_blob(K_SSID, c->ssid, sizeof(c->ssid) - 1, &blen) > 0)
        c->ssid[blen] = 0;
    if (ef_get_env_blob(K_PWD, c->pwd, sizeof(c->pwd) - 1, &blen) > 0)
        c->pwd[blen] = 0;
    if (ef_get_env_blob(K_CITY, c->city, sizeof(c->city) - 1, &blen) > 0)
        c->city[blen] = 0;
    if ((v = ef_get_env(K_PCIP)) != NULL && v[0])
        snprintf(c->pc_ip, sizeof(c->pc_ip), "%s", v);
    if ((v = ef_get_env(K_MON)) != NULL)
        c->mon_on = (v[0] == '1') ? 1 : 0;
    if ((v = ef_get_env(K_WXON)) != NULL)
        c->wx_on = (v[0] == '1') ? 1 : 0;
    if ((v = ef_get_env(K_WXAPI)) != NULL && v[0])
        snprintf(c->wx_api, sizeof(c->wx_api), "%s", v);
    if ((v = ef_get_env(K_AMAPK)) != NULL && v[0])
        snprintf(c->amap_key, sizeof(c->amap_key), "%s", v);
    if ((v = ef_get_env(K_QWKEY)) != NULL && v[0])
        snprintf(c->qw_key, sizeof(c->qw_key), "%s", v);
    if ((v = ef_get_env(K_QWCRED)) != NULL && v[0])
        snprintf(c->qw_cred, sizeof(c->qw_cred), "%s", v);
    if ((v = ef_get_env(K_CLK)) != NULL)
        c->clock_anim = (v[0] == '1') ? 1 : 0;
    if ((v = ef_get_env(K_WALL)) != NULL)
        c->wall_valid = (v[0] == '1') ? 1 : 0;
    if ((v = ef_get_env(K_WMODE)) != NULL)
        c->wall_mode = (v[0] == '1') ? 1 : 0;
    if ((v = ef_get_env(K_WSEC)) != NULL)
        c->wall_sec = (uint8_t)strtol(v, NULL, 10);
    if ((v = ef_get_env(K_WCLK)) != NULL)
        c->wall_clock_sec = (uint8_t)strtol(v, NULL, 10);
    if ((v = ef_get_env(K_BGC1)) != NULL) {
        uint32_t uv = (uint32_t)strtoul(v, NULL, 16);
        if (uv <= 0xFFFFFF) c->bg_c1 = uv;
    }
    if ((v = ef_get_env(K_BGC2)) != NULL) {
        uint32_t uv = (uint32_t)strtoul(v, NULL, 16);
        if (uv <= 0xFFFFFF) c->bg_c2 = uv;
    }
    if ((v = ef_get_env(K_BGDIR)) != NULL) {
        int d = (int)strtol(v, NULL, 10);
        if (d >= 0 && d <= 2) c->bg_dir = (uint8_t)d;
    }
    if ((v = ef_get_env(K_WANIM)) != NULL) {
        int a = (int)strtol(v, NULL, 10);
        if (a >= 0 && a <= 6) c->wall_anim = (uint8_t)a;
    }
    return 0;
}

int cfg_save(const cfg_t *c)
{
    char mon[2] = {c->mon_on ? '1' : '0', 0};
    char wxon[2] = {c->wx_on ? '1' : '0', 0};
    char clk[2] = {c->clock_anim ? '1' : '0', 0};
    char wall[2] = {c->wall_valid ? '1' : '0', 0};
    char wmode[2] = {c->wall_mode ? '1' : '0', 0};
    char wsec[4];
    char wclk[4];
    char wanim[4];
    char bgc1[9];
    char bgc2[9];
    char bgdir[2];
    snprintf(wsec, sizeof(wsec), "%u", c->wall_sec);
    snprintf(wclk, sizeof(wclk), "%u", c->wall_clock_sec);
    snprintf(wanim, sizeof(wanim), "%u", c->wall_anim);
    snprintf(bgc1, sizeof(bgc1), "%06x", (unsigned int)c->bg_c1);
    snprintf(bgc2, sizeof(bgc2), "%06x", (unsigned int)c->bg_c2);
    snprintf(bgdir, sizeof(bgdir), "%u", c->bg_dir);

    /* 全部只写内存 (ef_set_env* 不落盘), 最后统一 ef_save_env 一次持久化。
     * 之前每个键都用 ef_set_and_save_env = 20 次完整 flash 擦写 (~1.7 秒),
     * 配网 saveInfo 响应被阻塞且 flash 写期间无线丢包 → 手机显示"出错" */
    ef_set_env_blob(K_SSID, c->ssid, strlen(c->ssid));
    ef_set_env_blob(K_PWD,  c->pwd,  strlen(c->pwd));
    ef_set_env_blob(K_CITY, c->city, strlen(c->city));
    if (ef_set_env(K_PCIP, c->pc_ip) != EF_NO_ERR) return -1;
    if (ef_set_env(K_MON,  mon)      != EF_NO_ERR) return -1;
    if (ef_set_env(K_WXON, wxon)     != EF_NO_ERR) return -1;
    if (ef_set_env(K_WXAPI, c->wx_api) != EF_NO_ERR) return -1;
    if (ef_set_env(K_AMAPK, c->amap_key) != EF_NO_ERR) return -1;
    if (ef_set_env(K_QWKEY, c->qw_key) != EF_NO_ERR) return -1;
    if (ef_set_env(K_QWCRED, c->qw_cred) != EF_NO_ERR) return -1;
    if (ef_set_env(K_CLK,  clk)   != EF_NO_ERR) return -1;
    if (ef_set_env(K_WALL, wall)  != EF_NO_ERR) return -1;
    if (ef_set_env(K_WMODE, wmode) != EF_NO_ERR) return -1;
    if (ef_set_env(K_WSEC, wsec)  != EF_NO_ERR) return -1;
    if (ef_set_env(K_WCLK, wclk)  != EF_NO_ERR) return -1;
    if (ef_set_env(K_WANIM, wanim) != EF_NO_ERR) return -1;
    if (ef_set_env(K_BGC1, bgc1)  != EF_NO_ERR) return -1;
    if (ef_set_env(K_BGC2, bgc2)  != EF_NO_ERR) return -1;
    if (ef_set_env(K_BGDIR, bgdir) != EF_NO_ERR) return -1;
    if (ef_save_env() != EF_NO_ERR) return -1;   /* 一次持久化全部改动 */
    return 0;
}

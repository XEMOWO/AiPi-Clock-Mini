/**
 * @brief provision — 配网模式: 5× EN 复位触发, 软AP + captive portal + HTTP 配网页
 *
 * 复位计数 (真机验证: EN(CHIP_EN) 复位会清空整个 HBN 域, 寄存器方案不可行;
 * easyflash 方案会被启动时序/出厂重置干扰 → 最终用 bootcnt 纯 flash 扇区,
 * 在 bfl_main 极早期"上电第一时间"计数, 连续快速按 EN 5 次即进配网):
 *   - bootcnt 扇区: 累计开机次数, 每次启动 +1, >=5 → 进入配网模式
 *   - 配网成功 / 稳定运行 3 分钟 / SNTP 显示时间 → 清零计数
 *   - w_plast: 配网保持标志. 进入配网后置 1 → 期间按 EN / 断电重插都继续
 *     配网; 配网成功或 10 分钟无操作自动退出(清标志 + 复位)
 *
 * 配网流程:
 *   1. 软AP "Clock-Mini-XXXX" (MAC 后4位) / 12345678, IP 192.168.169.1
 *   2. DNS 重定向 (captive portal) → 手机连上后自动弹出配网页
 *   3. HTTP 配网页: SSID(可扫描) + 密码 → POST /saveInfo
 *   4. 保存到 easyflash (g_cfg) → STA 连接路由器
 *   5. GOT_IP → 弹窗提示 → GLB_SW_POR_Reset() 重启, 正常模式连新 WiFi
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <bl602_glb.h>
#include <aos/yloop.h>
#include <hal_wifi.h>
#include <wifi_mgmr_ext.h>
#include <lwip/inet.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/udp.h>
#include <lwip/pbuf.h>
#include <lwip/err.h>
#include <lwip/api.h>
#include <utils_dns.h>
#include <cJSON.h>
#include <easyflash.h>
#ifndef LV_FONT_MONTSERRAT_40
#define LV_FONT_MONTSERRAT_40 1   /* 配网页 AP 名 40px (custom/ 提供实现) */
#endif
#include "lvgl.h"
#include "provision.h"
#include "cfg_store.h"
#include "bootcnt.h"        /* 上电第一时间开机计数 (bfl_main 极早期, 纯 flash) */
#include "wx_icons.h"       /* lv_font_cn24 24px 中文字库 */
extern lv_font_t lv_font_montserrat_36;   /* custom/ 提供的 36px 字体 (AP 名) */

/* ============ 复位计数 (bootcnt 纯 flash 扇区, bfl_main 极早期) ============ */
/* PROV_NEED_COUNT 已移至 provision.h (main.c 出厂重置共用) */
#define PROV_KEEPALIVE_MS   (3 * 60 * 1000)   /* 正常稳定运行 3 分钟 → 清零计数 */

#define K_PLAST  "w_plast"   /* 配网保持标志 0/1 */

static int g_prov_active = 0;
static volatile int s_configured = 0;   /* 配网已成功(GOT_IP) */
static void prov_captive_start(void);
void prov_http_start(void);

extern int g_boot_count;   /* bootcnt.c: bfl_main 极早期已计数 (媒体扇区), -1=未计数 */

/* 正常模式清零开机计数: 稳定运行 3 分钟即视为正常使用, 防止日常重启/插拔
 * 累计到 5 次误进配网; 3 分钟窗口远大于连按 5 次复位(通常 <1 分钟) */
static void prov_keepalive_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(PROV_KEEPALIVE_MS));
    bootcnt_clear();
    printf("[PROV] count reset (stable run)\r\n");
    vTaskDelete(NULL);
}

void prov_keepalive_start(void)
{
    xTaskCreate(prov_keepalive_task, "prov_ka", 512, NULL, 2, NULL);
}

/* main 开头调用: 计数已在 bfl_main 极早期完成 (g_boot_count), 这里兜底重试 +
 * 判断是否进配网。返回 1=本次进入配网模式 */
int prov_boot_check(void)
{
    if (ef_get_env(K_PLAST) != NULL && ef_get_env(K_PLAST)[0] == '1') {
        /* 上次处于配网模式: 保持, 不计数 */
        g_prov_active = 1;
        printf("[PROV] resume provisioning\r\n");
        return 1;
    }

    /* 极早期计数失败(极端情况)时在 main 兜底重试一次 */
    int count = (g_boot_count >= 0) ? g_boot_count : bootcnt_inc();
    if (count >= PROV_NEED_COUNT) {
        g_prov_active = 1;
        bootcnt_clear();                /* 消费计数 */
        ef_set_and_save_env(K_PLAST, "1");  /* 保持配网模式 */
        printf("[PROV] enter provisioning (count=%d)\r\n", count);
        return 1;
    }
    printf("[PROV] boot count=%d\r\n", count);
    return 0;
}

int prov_is_active(void) { return g_prov_active; }

static void prov_gen_ap_ssid(void);

/* 运行时进入配网(正常模式超时无网时由 main.c 调用):
 * 与开机进配网的区别: 正常模式的 STA 可能正在自动重连, 先断开并关闭
 * 自动重连, 再切 UI + 启动 AP/captive portal; 置保持标志(重启后继续配网) */
void prov_enter(void)
{
    if (g_prov_active) return;
    g_prov_active = 1;
    ef_set_and_save_env(K_PLAST, "1");
    printf("[PROV] runtime enter provisioning\r\n");
    wifi_mgmr_sta_disconnect();
    wifi_mgmr_sta_autoconnect_disable();
    prov_ui_init();
    prov_gen_ap_ssid();   /* MGMR_DONE 已被 main.c 消费: 主动补 AP 名 + MAC */
    xTaskCreate(prov_start, "prov", 1024 * 4, NULL, 15, NULL);
}

/* ============ 配网 UI (英文, LVGL 内置 montserrat) ============ */
#define UI_W   320
#define UI_H   220
#define C_BG   0x1E1E2E
#define C_CARD 0x2A2A3E
#define C_ACC  0x7EC8FF
#define C_TXT  0xEEEEEE
#define C_SUB  0x9AA0B4
#define C_OK   0x4CAF50
#define C_ERR  0xF44336

static lv_obj_t *p_scr, *p_ap_lbl, *p_state_lbl;
extern SemaphoreHandle_t lvgl_mutex;    /* main.c 定义 */
extern int g_wifi_fw_started;           /* main.c 定义: 固件任务是否已启动 */
static char s_mac_str[18] = "--:--:--:--:--:--";   /* 配网页显示用, AP 启动时填充 */

static void prov_ui_create(void)
{
    lv_obj_t *card, *lbl;

    p_scr = lv_obj_create(NULL);
    lv_obj_set_size(p_scr, UI_W, UI_H);
    lv_obj_set_style_bg_color(p_scr, lv_color_hex(0x000000), 0);   /* 全黑背景 */
    lv_obj_set_style_bg_opa(p_scr, LV_OPA_COVER, 0);

    /* 第1行: 配网模式 (蓝色, 居中) */
    lbl = lv_label_create(p_scr);
    lv_obj_set_style_text_font(lbl, &lv_font_cn24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_ACC), 0);
    lv_label_set_text(lbl, "配网模式");
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 14);

    /* 第2行: AP 名称 (36px, 白色, 居中) */
    p_ap_lbl = lv_label_create(p_scr);
    lv_obj_set_style_text_font(p_ap_lbl, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(p_ap_lbl, lv_color_hex(C_TXT), 0);
    lv_label_set_text(p_ap_lbl, "Clock-Mini-????");
    lv_obj_align(p_ap_lbl, LV_ALIGN_TOP_MID, 0, 60);

    /* 第3行: 连接状态 (左对齐) */
    p_state_lbl = lv_label_create(p_scr);
    lv_obj_set_style_text_font(p_state_lbl, &lv_font_cn18, 0);
    lv_obj_set_style_text_color(p_state_lbl, lv_color_hex(C_SUB), 0);
    lv_label_set_long_mode(p_state_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(p_state_lbl, 288);
    lv_label_set_text(p_state_lbl, "打开WIFI, 搜索以上WIFI名称连接");
    lv_obj_align(p_state_lbl, LV_ALIGN_TOP_LEFT, 16, 150);
}

/* 状态更新: 只改文字, 颜色固定灰色不变 */
static void prov_ui_status(const char *msg, uint32_t color)
{
    (void)color;
    xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
    lv_label_set_text(p_state_lbl, msg);
    xSemaphoreGive(lvgl_mutex);
}

/* ============ WiFi 事件 / AP 状态机 ============ */
#define AP_PWD  "12345678"

static wifi_conf_t s_ap_conf = {.country_code = "CN"};
static wifi_interface_t s_ap_if;
static char s_ap_ssid[33];
static volatile int s_connecting = 0;   /* 已收到配置, STA 连接中 */
static volatile int s_conn_fail = 0;    /* STA 连接失败 (手机端 /getStatus 轮询用) */
static volatile int s_retry_left = 0;   /* STA 连接失败剩余自动重试次数 (saveInfo 时重置) */
static volatile int s_http_started = 0;
static volatile int s_ap_started = 0;   /* AP 是否已启动 (MGMR_DONE 兜底判断用) */
static int s_ap_chan = 6;               /* AP 当前信道 (prov_ap_start 初始 ch6) */
static int s_try_chan = 0;              /* 目标路由器信道: handle_save 扫到后记录,
                                         * 重试任务定向连接用 (0=未知, 全信道扫描) */

/* STA 连接偶发失败(4way 握手超时/扫描丢 AP)重连即愈; 但 SDK 自动重连在
 * 单射频 AP+STA 共存的配网模式下会风暴式耗尽描述符, 必须关闭, 由应用层
 * 按固定节奏有限次重试: 密码错时 2 次共 ~12s 反馈失败, 偶发失败则自动成功 */
#define PROV_RETRY_MAX  2      /* 最大自动重试次数 */
#define PROV_RETRY_DLY  6000   /* 重试间隔 ms (过短会与 AP 通路抢描述符) */

static void prov_retry_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(PROV_RETRY_DLY));
    /* 延迟窗口内可能已 GOT_IP 或用户重新提交: 状态不符则放弃重试 */
    if (g_prov_active && s_connecting && !s_configured && !s_conn_fail) {
        printf("[PROV] STA retry connect (%d left, chan %d)\r\n", s_retry_left, s_try_chan);
        wifi_interface_t iface = wifi_mgmr_sta_enable();
        wifi_mgmr_sta_connect(iface, g_cfg.ssid, g_cfg.pwd, NULL, NULL, 0, s_try_chan);
    }
    vTaskDelete(NULL);
}

static void prov_ap_start(void)
{
    struct netif *ap_netif;
    ip4_addr_t ip, mask;

    s_ap_if = wifi_mgmr_ap_enable();
    wifi_mgmr_conf_max_sta(4);
    wifi_mgmr_ap_start(s_ap_if, s_ap_ssid, 0, NULL, 6);   /* 开放网络, 无需密码 */
    s_ap_chan = 6;      /* 记录 AP 当前信道 (切换逻辑判断用) */

    /* 静态 IP + 系统 DHCP 服务 (lwip_dhcpd) */
    ap_netif = netif_find("ap1");
    if (ap_netif) {
        IP4_ADDR(&ip, 192, 168, 169, 1);
        IP4_ADDR(&mask, 255, 255, 255, 0);
        netif_set_down(ap_netif);
        netif_set_ipaddr(ap_netif, &ip);
        netif_set_netmask(ap_netif, &mask);
        netif_set_gw(ap_netif, &ip);
        netif_set_up(ap_netif);
    }
    s_ap_started = 1;
    printf("[PROV] AP \"%s\" / 192.168.169.1\r\n", s_ap_ssid);
}

/* 生成 AP 名 Clock-Mini-XXYY + 配网页 MAC 字符串。
 * 开机进配网时 MGMR_DONE 由 prov_wifi_cb 收到; 正常模式运行时进配网时
 * (连接超时), MGMR_DONE 已被 main.c 的 wifi_cb 消费, 必须主动补填 */
static void prov_gen_ap_ssid(void)
{
    uint8_t mac[6];
    char tmp[12];
    if (wifi_mgmr_sta_mac_get(mac) == 0) {
        snprintf(tmp, sizeof(tmp), "%02X%02X", mac[4], mac[5]);
        snprintf(s_mac_str, sizeof(s_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strcpy(tmp, "CONF");
    }
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Clock-Mini-%s", tmp);
    xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
    if (p_ap_lbl)
        lv_label_set_text(p_ap_lbl, s_ap_ssid);
    xSemaphoreGive(lvgl_mutex);
}

static void prov_wifi_cb(input_event_t *ev, void *pv)
{
    switch (ev->code) {
    case CODE_WIFI_ON_INIT_DONE:
        wifi_mgmr_start_background(&s_ap_conf);
        break;
    case CODE_WIFI_ON_MGMR_DONE:
        prov_gen_ap_ssid();
        if (!s_ap_started)   /* 兜底任务可能已抢先启动, 幂等 */
            prov_ap_start();
        break;
    case CODE_WIFI_ON_AP_STARTED:
        printf("[PROV] AP started, HTTP + captive DNS\r\n");
        prov_http_start();
        prov_captive_start();
        break;
    case CODE_WIFI_ON_AP_STA_ADD:
        printf("[PROV] phone connected\r\n");
        prov_ui_status("手机已连接, 设置页弹出中...", C_ACC);
        break;
    case CODE_WIFI_ON_GOT_IP:
        printf("[PROV] GOT IP, config OK, rebooting\r\n");
        s_configured = 1;
        s_conn_fail = 0;
        s_connecting = 0;   /* 已连上: 重启前窗口内 WiFi 短暂闪断不再误报配网失败 */
        if (bootcnt_clear() != 0) {         /* 配网完成: 清零计数 (失败必须 WARN) */
            printf("[PROV] WARN bootcnt clear failed\r\n");
        }
        ef_set_and_save_env(K_PLAST, "0");  /* 退出配网模式 */
        prov_ui_status("配网成功, 即将重启!", C_OK);
        vTaskDelay(pdMS_TO_TICKS(8000));    /* 留窗口给手机端 /getStatus 轮询到 ok
                                             * (STA 连接+DHCP 需 2~6s, 窗口必须大于此) */
        GLB_SW_POR_Reset();
        break;
    case CODE_WIFI_ON_DISCONNECT:
        if (s_connecting && !s_configured) {   /* 已配网成功(等重启)时忽略断线 */
            int sc = 0;
            wifi_mgmr_status_code_get(&sc);
            wifi_mgmr_sta_autoconnect_disable(); /* 停止自动重连风暴: 否则 WiFi 固件
                                                  * 描述符耗尽 (No enough DESC), AP 数据
                                                  * 通路被堵死, 手机轮询不到失败状态 */
            printf("[PROV] STA connect failed (code %d)\r\n", sc);
            /* 只有密码错才不重试, 立即反馈失败 (重试也没用, 白等 12s):
             *   sc 8 = WLAN_FW_4WAY_HANDSHAKE_ERROR_PSK_TIMEOUT_FAILURE
             *          (SDK 状态码串: "Passwd error, 4-way handshake timeout")
             *   sc 6 = WLAN_FW_DEAUTH_BY_AP_WHEN_NOT_CONNECTION 是偶发:
             *          单射频 AP+STA 异信道瞬间 (AP 刚切完/手机跟随间隙) 路由器
             *          会在 DHCP 阶段踢 STA, 重试即愈 —— 不能判密码错
             * 其余状态码也视为偶发失败(路由器不在线/扫描丢 AP/握手被干扰),
             * 自动重试有限次 */
            if (sc == 8) {
                s_connecting = 0;
                s_conn_fail = 1;            /* 手机端 /getStatus 轮询 → fail */
                prov_ui_status("连接失败, 请检查密码", C_ERR);
            } else if (s_retry_left > 0) {
                /* 偶发失败自动重试: 期间保持 connecting 状态,
                 * 手机端继续轮询等待, 重试耗尽仍未成功才报 fail */
                s_retry_left--;
                prov_ui_status("连接失败, 正在重连...", C_ACC);
                xTaskCreate(prov_retry_task, "prov_retry", 1024 * 2, NULL, 10, NULL);
            } else {
                s_connecting = 0;
                s_conn_fail = 1;
                prov_ui_status("连接失败, 请检查密码", C_ERR);
            }
        }
        break;
    default:
        break;
    }
}

/* MGMR_DONE 兜底: 开机直进配网时固件任务刚启动, 若 6s 内 MGMR_DONE 事件
 * 偶发丢失, AP 不会启动、界面 AP 名停留在占位符 —— 到时主动补 AP 名 +
 * 启动 AP, 保证手机一定能搜到网络 */
static void prov_ap_retry_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(6000));
    if (g_prov_active && !s_ap_started) {
        printf("[PROV] MGMR_DONE timeout, force AP start\r\n");
        prov_gen_ap_ssid();
        prov_ap_start();
    }
    vTaskDelete(NULL);
}

/* 配网模式入口 (替代 main.c 的 wifi_entry): 注册事件 + 启动固件任务。
 * 配网模式永久保持: 不设超时自动退出, 直到配网成功重启
 * (或 5×EN 强退不适用 —— 配网中无计数; 断电重启后 PLAST=1 恢复配网)
 *
 * 开机直进配网(PLAST=1 恢复)时固件任务未启动, 走初始化分支;
 * 正常模式运行时 45s 超时进配网时, 固件任务/PM 已由 main.c 的
 * wifi_entry 启动, 重复调用 hal_wifi_start_firmware_task 会触发
 * bl_pm_init 的 assert(!gp_pm_env) 崩溃 —— 此时直接启 AP */
void prov_start(void *arg)
{
    (void)arg;
    aos_register_event_filter(EV_WIFI, prov_wifi_cb, NULL);
    wifi_mgmr_scan_filter_hidden_ssid(0);   /* 不过滤隐藏 SSID: 已配置的隐藏 WiFi 也要能扫到 */
    if (!g_wifi_fw_started) {
        g_wifi_fw_started = 1;
        hal_wifi_start_firmware_task();
        aos_post_event(EV_WIFI, CODE_WIFI_ON_INIT_DONE, 0);
        xTaskCreate(prov_ap_retry_task, "prov_aprt", 1024, NULL, 8, NULL);
    } else {
        /* 运行时进配网: mgmr 已在运行, MGMR_DONE 不会再发,
         * prov_enter 已填好 s_ap_ssid, 直接开 AP */
        prov_ap_start();
    }
    vTaskDelete(NULL);
}

/* ============ captive portal: DNS 重定向到 AP IP ============ */
/* 按 RFC1035 构造: header + question(域名+type/class) + answer(压缩指针+A记录) */
#define DNS_HDR_SZ   12
#define DNS_QTAIL_SZ 4    /* question 的 qtype+qclass */
#define DNS_ANS_SZ   16   /* answer: ptr(2)+type(2)+class(2)+ttl(4)+rdlen(2)+rdata(4) */

struct prov_dns_ctx {
    uint8_t name[256];
    uint16_t txid;
    struct udp_pcb *upcb1;
    const ip_addr_t *addr1;
    u16_t port1;
};

static void prov_dns_reply(struct prov_dns_ctx *ctx)
{
    struct netif *ap_netif = netif_find("ap1");
    ip4_addr_t dns_ip = {0};
    struct pbuf *rp;
    struct {
        uint16_t id, flag, numq, numa, numau, numex;
    } hdr;
    uint16_t qidx;
    uint32_t n;
    const char *host, *part;

    if (ap_netif) {
        dns_ip = *netif_ip4_addr(ap_netif);
    }

    rp = pbuf_alloc(PBUF_TRANSPORT, 512, PBUF_RAM);
    if (!rp) {
        return;
    }
    memset(&hdr, 0, sizeof(hdr));
    hdr.id    = htons(ctx->txid);
    hdr.flag  = htons(0x8180);      /* QR=1 RD=1 RA=1 no error */
    hdr.numq  = htons(1);
    hdr.numa  = htons(1);
    pbuf_take(rp, &hdr, DNS_HDR_SZ);

    /* question: 域名 + qtype=A + qclass=IN */
    host = (const char *)ctx->name;
    host--;
    qidx = DNS_HDR_SZ;
    do {
        host++;
        part = host;
        for (n = 0; *host != '.' && *host != 0; host++) {
            n++;
        }
        pbuf_put_at(rp, qidx, (uint8_t)n);
        pbuf_take_at(rp, part, (u16_t)n, qidx + 1);
        qidx += (uint16_t)(n + 1);
    } while (*host != 0);
    pbuf_put_at(rp, qidx, 0);
    qidx++;
    pbuf_put_at(rp, qidx, 0); pbuf_put_at(rp, qidx + 1, 1);      /* qtype  */
    pbuf_put_at(rp, qidx + 2, 0); pbuf_put_at(rp, qidx + 3, 1);  /* qclass */
    qidx += DNS_QTAIL_SZ;

    /* answer: 压缩指针回指问题域 + A 记录 */
    pbuf_put_at(rp, qidx, 0xC0); pbuf_put_at(rp, qidx + 1, 0x0C);
    pbuf_put_at(rp, qidx + 2, 0); pbuf_put_at(rp, qidx + 3, 1);      /* type A   */
    pbuf_put_at(rp, qidx + 4, 0); pbuf_put_at(rp, qidx + 5, 1);      /* class IN */
    pbuf_put_at(rp, qidx + 6, 0); pbuf_put_at(rp, qidx + 7, 0);      /* ttl */
    pbuf_put_at(rp, qidx + 8, 0); pbuf_put_at(rp, qidx + 9, 60);
    pbuf_put_at(rp, qidx + 10, 0); pbuf_put_at(rp, qidx + 11, 4);    /* rdlen */
    pbuf_put_at(rp, qidx + 12, (uint8_t)(dns_ip.addr & 0xFF));
    pbuf_put_at(rp, qidx + 13, (uint8_t)((dns_ip.addr >> 8) & 0xFF));
    pbuf_put_at(rp, qidx + 14, (uint8_t)((dns_ip.addr >> 16) & 0xFF));
    pbuf_put_at(rp, qidx + 15, (uint8_t)((dns_ip.addr >> 24) & 0xFF));
    pbuf_realloc(rp, qidx + DNS_ANS_SZ);

    udp_sendto(ctx->upcb1, rp, ctx->addr1, ctx->port1);
    pbuf_free(rp);
}

static void prov_dns_recv(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                          const ip_addr_t *addr, u16_t port)
{
    struct prov_dns_ctx ctx;
    struct { uint16_t id; } hdr;
    int len;

    (void)arg;
    memset(&ctx, 0, sizeof(ctx));
    ctx.upcb1 = upcb;
    ctx.addr1 = addr;
    ctx.port1 = port;

    if (p->tot_len >= DNS_HDR_SZ) {
        pbuf_copy_partial(p, &hdr, 2, 0);
        ctx.txid = ntohs(hdr.id);
        pbuf_copy_partial(p, ctx.name,
                          len = p->tot_len > (int)sizeof(ctx.name) - 1
                                    ? (int)sizeof(ctx.name) - 1 : p->tot_len,
                          DNS_HDR_SZ);
        if (0 == utils_dns_domain_get(ctx.name, ctx.name, &len)) {
            if (len > 0 && '.' == ctx.name[len - 1]) {
                len--;
            }
            ctx.name[len] = '\0';
        }
        prov_dns_reply(&ctx);
    }
    pbuf_free(p);
}

static struct udp_pcb *s_captive_pcb;

static void prov_captive_start(void)
{
    if (s_captive_pcb) {
        return;
    }
    s_captive_pcb = udp_new();
    if (!s_captive_pcb) {
        return;
    }
    if (udp_bind(s_captive_pcb, IP_ADDR_ANY, 53) != ERR_OK) {
        udp_remove(s_captive_pcb);
        s_captive_pcb = NULL;
        return;
    }
    udp_recv(s_captive_pcb, prov_dns_recv, NULL);
    printf("[PROV] captive DNS up (port 53)\r\n");
}

/* ============ HTTP 配网页 ============ */
/* 内联 HTML: 手机端自适应, SSID 扫描下拉 + 密码 + 保存 */
/* 配网页顶部 logo: 安信可, 白字透明底 PNG base64(内联, 无需额外接口) */
#define PROV_LOGO_B64 "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"
static const char PROV_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">\n<meta name=\"viewport\" content="
    "\"width=device-width,initial-scale=1\">\n<title>AiPi-Clock-Mini 配网</title><link rel=\"icon\" href=\"data:image/png;base64,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\"><style>\nbody{fo"
    "nt-family:system-ui,Arial;background:#12122a;color:#eee;\npadding:16px;max-width:"
    "420px;margin:0 auto}\nh1{font-size:20px;color:#7ec8ff;margin:8px 0 2px}\np{color:#"
    "9aa0b4;font-size:13px;margin:4px 0 12px}\nlabel{display:block;margin:12px 0 4px;f"
    "ont-size:13px;color:#9aa}\ninput,select{width:100%;padding:10px;border:1px solid "
    "#3a3a5a;\nborder-radius:8px;background:#1e1e3a;color:#eee;font-size:16px;\nbox-siz"
    "ing:border-box}\nbutton{width:100%;padding:12px;margin-top:18px;border:0;border-r"
    "adius:8px;\nbackground:#2f9e5f;color:#fff;font-size:16px}\n.row{display:flex;gap:8"
    "px;margin-top:12px}\n.row input{flex:1}.row button{width:auto;margin:0;padding:10"
    "px 14px;\nbackground:#3a3a6a}\n#wlist,#clist{display:none;max-height:170px;overflo"
    "w-y:auto;margin-top:8px;\nborder:1px solid #3a3a5a;border-radius:8px;background:#"
    "1e1e3a}\n#wlist .opt,#clist .opt{padding:10px 12px;border-bottom:1px solid #3a3a5"
    "a;\ncolor:#eee;font-size:15px;word-break:break-all}\n#wlist .opt:last-child,#clist"
    " .opt:last-child{border-bottom:0}\n#msg{text-align:center;margin-top:14px;color:#"
    "ffd166;min-height:20px;\nfont-size:14px;word-break:break-all}\n.bars{display:inline-flex;align-items:flex-end;gap:1px;margin-right:6px;vertical-align:middle}\n.bars i{width:4px;height:15px;background:currentColor;border-radius:1px}\n.bars i:nth-child(1){height:5px}\n.bars i:nth-child(2){height:9px}\n.bars i:nth-child(3){height:12px}\n.bars i:nth-child(4){height:15px}\n.bars.s4{color:#4caf50}\n.bars.s3{color:#ffd166}\n.bars.s2{color:#ffa94d}\n.bars.s1{color:#9aa0b4}\n</style></head><bod"
    "y>\n<script>setTimeout(function(){try{if(typeof scan!=='function'){var n=(sessionStorage.pv_r|0)+1;sessionStorage.pv_r=n;if(n<=5)location.reload()}}catch(e){}},2500)</script>\n<div style=\"text-align:center;margin:6px 0 2px\"><img src=\"data:image/png;base64," PROV_LOGO_B64 "\" style=\"height:40px\"></div>\n<h1>AiPi-Clock-Mini &mdash; 配网</h1>\n<p>选择路由器WiFi, 输入密码后连接。</p>\n<label>路由器WiFi</labe"
    "l>\n<div class=\"row\"><input id=\"ssid\" placeholder=\"输入或选择WiFi名\" autocomplete=\"off\""
    ">\n<button onclick=\"toggleList()\">&#9660;</button>\n<button onclick=\"scan()\">扫描</b"
    "utton></div>\n<div id=\"wlist\"></div>\n<label>密码</label>\n<div class=\"row\"><input id=\"pwd\" type=\"password"
    "\" placeholder=\"路由器密码\"><button type=\"button\" onclick=\"togglePwd()\">&#128065;</button></div>\n<!-- 城市选择已注释(出厂不带天气功能): <label>城市 (用于天气)</label>\n<div class=\"row\"><input id=\"city"
    "\" placeholder=\"输入或选择城市\" autocomplete=\"off\" oninput=\"cityFilter()\">\n<button oncli"
    "ck=\"toggleCity()\">&#9660;</button></div>\n<div id=\"clist\"></div> -->\n<button id=\"btnSave\" onclick="
    "\"save()\">连接</button>\n<div id=\"msg\"></div><script>\nvar CITIES=[\"石家庄\", \"唐山\", \"秦皇岛\""
    ", \"邯郸\", \"邢台\", \"保定\", \"张家口\", \"承德\", \"沧州\", \"廊坊\", \"衡水\", \"太原\", \"大同\", \"阳泉\", \"长治\", \"晋城\","
    " \"朔州\", \"晋中\", \"运城\", \"忻州\", \"临汾\", \"吕梁\", \"呼和浩特\", \"包头\", \"乌海\", \"赤峰\", \"通辽\", \"鄂尔多斯\", \"呼伦"
    "贝尔\", \"巴彦淖尔\", \"乌兰察布\", \"兴安盟\", \"锡林郭勒盟\", \"阿拉善盟\", \"沈阳\", \"大连\", \"鞍山\", \"抚顺\", \"本溪\", \"丹东\","
    " \"锦州\", \"营口\", \"阜新\", \"辽阳\", \"盘锦\", \"铁岭\", \"朝阳\", \"葫芦岛\", \"长春\", \"吉林\", \"四平\", \"辽源\", \"通化\", "
    "\"白山\", \"松原\", \"白城\", \"延边朝鲜族自治州\", \"哈尔滨\", \"齐齐哈尔\", \"鸡西\", \"鹤岗\", \"双鸭山\", \"大庆\", \"伊春\", \"佳木斯"
    "\", \"七台河\", \"牡丹江\", \"黑河\", \"绥化\", \"大兴安岭地区\", \"南京\", \"无锡\", \"徐州\", \"常州\", \"苏州\", \"南通\", \"连云港\""
    ", \"淮安\", \"盐城\", \"扬州\", \"镇江\", \"泰州\", \"宿迁\", \"杭州\", \"宁波\", \"温州\", \"嘉兴\", \"湖州\", \"绍兴\", \"金华\", "
    "\"衢州\", \"舟山\", \"台州\", \"丽水\", \"合肥\", \"芜湖\", \"蚌埠\", \"淮南\", \"马鞍山\", \"淮北\", \"铜陵\", \"安庆\", \"黄山\", \""
    "滁州\", \"阜阳\", \"宿州\", \"六安\", \"亳州\", \"池州\", \"宣城\", \"福州\", \"厦门\", \"莆田\", \"三明\", \"泉州\", \"漳州\", \"南平"
    "\", \"龙岩\", \"宁德\", \"南昌\", \"景德镇\", \"萍乡\", \"九江\", \"新余\", \"鹰潭\", \"赣州\", \"吉安\", \"宜春\", \"抚州\", \"上饶\""
    ", \"济南\", \"青岛\", \"淄博\", \"枣庄\", \"东营\", \"烟台\", \"潍坊\", \"济宁\", \"泰安\", \"威海\", \"日照\", \"临沂\", \"德州\", "
    "\"聊城\", \"滨州\", \"菏泽\", \"郑州\", \"开封\", \"洛阳\", \"平顶山\", \"安阳\", \"鹤壁\", \"新乡\", \"焦作\", \"濮阳\", \"许昌\", \""
    "漯河\", \"三门峡\", \"南阳\", \"商丘\", \"信阳\", \"周口\", \"驻马店\", \"武汉\", \"黄石\", \"十堰\", \"宜昌\", \"襄阳\", \"鄂州\", \""
    "荆门\", \"孝感\", \"荆州\", \"黄冈\", \"咸宁\", \"随州\", \"恩施土家族苗族自治州\", \"长沙\", \"株洲\", \"湘潭\", \"衡阳\", \"邵阳\", \""
    "岳阳\", \"常德\", \"张家界\", \"益阳\", \"郴州\", \"永州\", \"怀化\", \"娄底\", \"湘西土家族苗族自治州\", \"广州\", \"韶关\", \"深圳\", "
    "\"珠海\", \"汕头\", \"佛山\", \"江门\", \"湛江\", \"茂名\", \"肇庆\", \"惠州\", \"梅州\", \"汕尾\", \"河源\", \"阳江\", \"清远\", \"东"
    "莞\", \"中山\", \"潮州\", \"揭阳\", \"云浮\", \"南宁\", \"柳州\", \"桂林\", \"梧州\", \"北海\", \"防城港\", \"钦州\", \"贵港\", \"玉林"
    "\", \"百色\", \"贺州\", \"河池\", \"来宾\", \"崇左\", \"海口\", \"三亚\", \"三沙\", \"儋州\", \"成都\", \"自贡\", \"攀枝花\", \"泸州\""
    ", \"德阳\", \"绵阳\", \"广元\", \"遂宁\", \"内江\", \"乐山\", \"南充\", \"眉山\", \"宜宾\", \"广安\", \"达州\", \"雅安\", \"巴中\", "
    "\"资阳\", \"阿坝藏族羌族自治州\", \"甘孜藏族自治州\", \"凉山彝族自治州\", \"贵阳\", \"六盘水\", \"遵义\", \"安顺\", \"毕节\", \"铜仁\", \"黔"
    "西南布依族苗族自治州\", \"黔东南苗族侗族自治州\", \"黔南布依族苗族自治州\", \"昆明\", \"曲靖\", \"玉溪\", \"保山\", \"昭通\", \"丽江\", \"普洱"
    "\", \"临沧\", \"楚雄彝族自治州\", \"红河哈尼族彝族自治州\", \"文山壮族苗族自治州\", \"西双版纳傣族自治州\", \"大理白族自治州\", \"德宏傣族景颇族自"
    "治州\", \"怒江傈僳族自治州\", \"迪庆藏族自治州\", \"拉萨\", \"日喀则\", \"昌都\", \"林芝\", \"山南\", \"那曲\", \"阿里地区\", \"西安\", \""
    "铜川\", \"宝鸡\", \"咸阳\", \"渭南\", \"延安\", \"汉中\", \"榆林\", \"安康\", \"商洛\", \"兰州\", \"嘉峪关\", \"金昌\", \"白银\", \"天"
    "水\", \"武威\", \"张掖\", \"平凉\", \"酒泉\", \"庆阳\", \"定西\", \"陇南\", \"临夏回族自治州\", \"甘南藏族自治州\", \"西宁\", \"海东\", "
    "\"海北藏族自治州\", \"黄南藏族自治州\", \"海南藏族自治州\", \"果洛藏族自治州\", \"玉树藏族自治州\", \"海西蒙古族藏族自治州\", \"银川\", \"石嘴山\""
    ", \"吴忠\", \"固原\", \"中卫\", \"乌鲁木齐\", \"克拉玛依\", \"吐鲁番\", \"哈密\", \"昌吉回族自治州\", \"博尔塔拉蒙古自治州\", \"巴音郭楞蒙古"
    "自治州\", \"阿克苏地区\", \"克孜勒苏柯尔克孜自治州\", \"喀什地区\", \"和田地区\", \"伊犁哈萨克自治州\", \"塔城地区\", \"阿勒泰地区\", \"自治区直"
    "辖县级行政区划\"];\nvar tries=0;\nfunction scan(){tries=0;document.getElementById('msg').t"
    "extContent='扫描中...';\nfetch('/scan').then(function(){poll()}).catch(function(){\nd"
    "ocument.getElementById('msg').textContent='扫描失败, 点击重试'})}\nfunction poll(){tries+"
    "+;\nfetch('/getNetWork').then(function(r){return r.json()}).then(function(j){\nif("
    "j.data&&j.data.length>0){loadList(j)}\nelse if(tries<40){setTimeout(poll,800)}\nel"
    "se{document.getElementById('msg').textContent='未找到WiFi, 点击重新扫描'}\n}).catch(functi"
    "on(){document.getElementById('msg').textContent='出错, 点击重试'})}\nfunction loadList("
    "j){\nvar w=document.getElementById('wlist');w.innerHTML='';\nfor(var i=0;i<j.data."
    "length;i++){\n(function(o){var d=document.createElement('div');d.className='opt';"
    "\nvar n=o.rssi>=-50?4:o.rssi>=-60?3:o.rssi>=-70?2:1;\nvar b=document.createElement('span');b.className='bars s'+n;b.innerHTML='<i></i><i></i><i></i><i></i>';\nd.appendChild(b);d.appendChild(document.createTextNode(o.name));\nd.onclick=function(){document.getElementById('ssid').value=o.name;\n"
    "w.style.display='none'};w.appendChild(d)})(j.data[i])}\nvar cur=document.getEleme"
    "ntById('ssid').value;\nif(!cur&&j.data.length>0)document.getElementById('ssid').v"
    "alue=j.data[0].name;\nw.style.display='block';\ndocument.getElementById('msg').textCont"
    "ent=\n'点击选择或输入WiFi, 输入密码后连接'}\nfunction toggleList(){\nvar w=document.getElementByI"
    "d('wlist');\nif(w.innerHTML){w.style.display=w.style.display==='block'?'none':'bl"
    "ock'}}\nfunction cityFilter(){\nvar q=document.getElementById('city').value;\nvar w"
    "=document.getElementById('clist');w.innerHTML='';var n=0;\nfor(var i=0;i<CITIES.l"
    "ength&&n<50;i++){\nif(q&&CITIES[i].indexOf(q)<0)continue;\n(function(c){var d=docu"
    "ment.createElement('div');d.className='opt';\nd.textContent=c;\nd.onclick=function"
    "(){document.getElementById('city').value=c;\nw.style.display='none'};w.appendChil"
    "d(d)})(CITIES[i]);n++}\nw.style.display='block'}\nfunction toggleCity(){\nvar w=doc"
    "ument.getElementById('clist');\nif(w.style.display==='block'){w.style.display='no"
    "ne';return}\ndocument.getElementById('city').value='';cityFilter()}\nfunction save"
    "(){var s=document.getElementById('ssid').value;\nvar p=document.getElementById('p"
    "wd').value;\nvar ce=document.getElementById('city');var c=ce?ce.value:'';\nif(!s){document.getElem"
    "entById('msg').textContent='请输入WiFi名';return}\ndocument.getElementById('btnSave').disabled=true;\ndocument.getElementById('msg').tex"
    "tContent='正在保存并连接 WiFi, 约需10秒...';\nvar body={NEWORK:{Network:s,Password:p}};\nif(c)body.CITY=c;\n"
    "var ctl2=new AbortController();\nvar tm2=setTimeout(function(){ctl2.abort()},12000);\nf"
    "etch('/saveInfo',{method:'POST',headers:{'Content-Type':'application/json'},\n"
    "signal:ctl2.signal,body:JSON.stringify(body)}).then(function(r){\nreturn r.json()}).then(function(j){\n"
    "clearTimeout(tm2);\nif(j.status==='ok'){\ndocument.getElementById('msg').textContent='✅ 已保存! 正在连接 WiFi...';\n"
    "pollStatus()\n}else{document.getElementById('btnSave').disabled=false;\ndocument.getElementById('msg').textContent='错误: '+j.error}\n"
    "}).catch(function(){\nclearTimeout(tm2);\ndocument.getElementById('msg').textContent='通信中断, 设备可能已保存, 正在重启...';\npollStatus()})}\nvar pGen=0;\nfunction pollStatus(){\nvar fails=0;\nvar myGen=++pGen;\n(function poll(){\n"
    "var ctl=new AbortController();\nvar tm=setTimeout(function(){ctl.abort()},4000);\n"
    "fetch('/getStatus',{signal:ctl.signal})\n.then(function(r){return r.json()}).then(function(j){\n"
    "clearTimeout(tm);\nfails=0;\nif(j.status==='ok'){\n"
    "document.getElementById('msg').textContent='✅ 配网成功! 设备正在重启...';\n"
    "setTimeout(function(){window.location.href='http://connectivitycheck.gstatic.com/generate_204'},1200)\n"
    "}else if(j.status==='fail'){\n/* 设备明确回报失败(密码错等): 显示失败但继续轮询, 设备恢复或重新提交后自动翻成成功 */\n"
    "document.getElementById('btnSave').disabled=false;\n"
    "document.getElementById('msg').textContent='连接失败, 请检查 WiFi 名和密码';\nif(myGen==pGen)setTimeout(poll,1000)\n"
    "}else{\ndocument.getElementById('msg').textContent='正在连接 WiFi, 请稍候...';\nif(myGen==pGen)setTimeout(poll,1000)\n"
    "}\n}).catch(function(e){\n/* 请求超时或连不上: 设备写flash/扫WiFi时 AP 会短暂无响应, 不算重启。\n配网 AP 永不自动关闭 → 长时间完全连不上 = 设备已重启 = 成功, 绝不是密码错 */\nclearTimeout(tm);\n"
    "fails++;\nif(!navigator.onLine){\ndocument.getElementById('msg').textContent='✅ 配网成功! 设备正在重启...'\n"
    "}else if(fails>=25){\ndocument.getElementById('msg').textContent='✅ 配网成功! 设备已重启'\n"
    "}else if(fails>=10){\ndocument.getElementById('msg').textContent='设备重启中, 请稍候...'\n"
    "}else{\ndocument.getElementById('msg').textContent='正在连接 WiFi...'\n}\nif(myGen==pGen)setTimeout(poll,1000)\n"
    "})\n})()\n}"
    "function togglePwd(){var p=document.getElementById('pwd');p.blur();p.type=p.type==='password'?'text':'password';setTimeout(function(){p.blur()},100)}\nwindow.onload=function(){};\n</script>\n<div style=\"text-align:center;color:#5a5a7a;font-size:11px;margin-top:16px\">xemowo &middot; v{{VER}} &middot; {{MAC}}</div>\n</body></html>";

#define REQ_BUF_SZ 4096

typedef struct {
    char ssids[50][33];
    uint8_t chans[50];
    int8_t rssis[50];   /* 信号强度 (配网页列表显示信号格) */
    int count;
} prov_scan_ctx_t;

static prov_scan_ctx_t s_scan;
static volatile int s_scanning = 0;
static SemaphoreHandle_t s_scan_sem = NULL;   /* 扫描完成信号 (saveInfo 信道匹配等待用) */

static int ssid_in_list(const char *ssid)
{
    int i;
    for (i = 0; i < s_scan.count; i++) {
        if (strcmp(s_scan.ssids[i], ssid) == 0) {
            return 1;
        }
    }
    return 0;
}

static void scan_item_cb(wifi_mgmr_ap_item_t *env, uint32_t *p1, wifi_mgmr_ap_item_t *item)
{
    (void)env; (void)p1;
    char ss[33];
    if (!item || s_scan.count >= 50 || item->ssid_len == 0) {
        return;
    }
    memset(ss, 0, sizeof(ss));
    memcpy(ss, item->ssid, item->ssid_len < 32 ? item->ssid_len : 32);
    if (ss[0] == 0 || ssid_in_list(ss)) {
        return;
    }
    strncpy(s_scan.ssids[s_scan.count], ss, 32);
    s_scan.ssids[s_scan.count][32] = 0;
    s_scan.chans[s_scan.count] = item->channel;
    s_scan.rssis[s_scan.count] = item->rssi;
    s_scan.count++;
}

static void scan_done_cb(void *data, void *param)
{
    (void)data; (void)param;
    memset(&s_scan, 0, sizeof(s_scan));
    wifi_mgmr_scan_ap_all(NULL, NULL, scan_item_cb);
    printf("[PROV] scan done, %d APs\r\n", s_scan.count);
    s_scanning = 0;
    if (s_scan_sem) {
        xSemaphoreGive(s_scan_sem);     /* 唤醒等待扫描结果的任务 (如有) */
    }
}

static void prov_scan_refresh(void)
{
    static uint16_t chans[13];
    static uint8_t zbssid[6] = {0};     /* SDK scan_adv 对 bssid 不做 NULL 检查, 必须传有效指针 */
    int i;

    if (s_scanning) {
        return;
    }
    s_scanning = 1;
    /* 快扫: 全信道 1-13, 每信道 80ms (wifi_mgmr_scan 默认 ~300ms/信道, 全信道
     * 实测 4 秒; 80ms 主动扫描对正常路由器足够, 总耗时 ~1.5s) */
    for (i = 0; i < 13; i++) {
        chans[i] = (uint16_t)(i + 1);
    }
    wifi_mgmr_scan_adv(NULL, scan_done_cb, chans, 13, zbssid, NULL, 1 /*SCAN_ACTIVE*/, 80000);
}

/* 等待一次扫描完成 (结果在 s_scan): 没有在扫则自己发起, 最多等 ~4s。
 * 返回 0=已拿到最新扫描结果; -1=超时/信号量创建失败 (调用方按原信道连接) */
static int prov_scan_wait(void)
{
    if (!s_scan_sem) {
        s_scan_sem = xSemaphoreCreateBinary();
        if (!s_scan_sem) {
            return -1;
        }
    }
    xSemaphoreTake(s_scan_sem, 0);          /* 清掉遗留信号量 */
    if (!s_scanning) {
        prov_scan_refresh();
    }
    if (s_scanning && xSemaphoreTake(s_scan_sem, pdMS_TO_TICKS(4000)) != pdTRUE) {
        return -1;
    }
    return 0;
}

/* 在最近一次扫描结果里找目标 SSID 的信道, 0=未找到/无结果 */
static int prov_scan_match_channel(const char *ssid)
{
    int i;
    if (prov_scan_wait() != 0) {
        return 0;
    }
    for (i = 0; i < s_scan.count; i++) {
        if (strcmp(s_scan.ssids[i], ssid) == 0) {
            return s_scan.chans[i];
        }
    }
    return 0;
}

static const char hdr_html[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
    "Cache-Control: no-store\r\n"
    "Connection: close\r\n\r\n";    /* no-store: 配网页不缓存, 固件升级后手机
                                     * 重进配网总是拿到新页面/新 JS */
static const char hdr_json[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
    "Connection: close\r\n\r\n";
static const char hdr_204[] =
    "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
static const char hdr_404[] =
    "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
    "Connection: close\r\n\r\n";
static const char hdr_cors[] =
    "HTTP/1.1 204 No Content\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
    "Access-Control-Allow-Headers: Content-Type\r\n"
    "Connection: close\r\n\r\n";    /* CORS 预检: 页面(80)跨域 fetch 8080 时浏览器先发
                                     * OPTIONS, 必须放行否则 POST 被浏览器拦截 */
static const char hdr_badreq[] =
    "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=utf-8\r\n"
    "Connection: close\r\n\r\n{\"error\":\"bad request\"}";

/* iOS/Android/厂商 自动探活路径 → 直接返回配网页
 * 注意: 部分系统/App 的探活带 UUID 后缀 (如 /generate_204_<uuid>), 前缀匹配 */
static int path_is_captive_probe(const char *path)
{
    if (!path || path[0] != '/') {
        return 0;
    }
    if (!strcmp(path, "/") || !strcasecmp(path, "/index.html")) {
        return 1;
    }
    if (strncmp(path, "/generate_204", 13) == 0) {
        return 1;
    }
    return !strcmp(path, "/hotspot-detect.html") || !strcmp(path, "/connecttest.txt")
        || !strcmp(path, "/redirect") || !strcmp(path, "/success.txt")
        || !strcmp(path, "/ncsi.txt") || !strcmp(path, "/library/test/success.html")
        || !strcmp(path, "/hotspot-detect.html") || !strcmp(path, "/canonical.html");
}

static void send_raw(struct netconn *conn, const void *data, size_t len)
{
    netconn_write(conn, data, len, NETCONN_COPY);
}

/* 发送配网页: 分段写入并替换 {{VER}}/{{MAC}} 占位符(避免大 RAM 缓冲) */
static void prov_send_page(struct netconn *conn)
{
    const char *v1 = strstr(PROV_HTML, "{{VER}}");
    const char *v2 = strstr(PROV_HTML, "{{MAC}}");
    size_t total = sizeof(PROV_HTML) - 1;

    if (!v1 || !v2) {   /* 占位符缺失: 原样发送 */
        send_raw(conn, PROV_HTML, total);
        return;
    }
    send_raw(conn, PROV_HTML, (size_t)(v1 - PROV_HTML));
    send_raw(conn, FW_VER_STR, strlen(FW_VER_STR));
    send_raw(conn, v1 + 7, (size_t)(v2 - (v1 + 7)));
    send_raw(conn, s_mac_str, strlen(s_mac_str));
    send_raw(conn, v2 + 7, total - (size_t)(v2 + 7 - PROV_HTML));
}

static void reply_json(struct netconn *conn, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        send_raw(conn, hdr_404, sizeof(hdr_404) - 1);
        return;
    }
    send_raw(conn, hdr_json, sizeof(hdr_json) - 1);
    send_raw(conn, s, strlen(s));
    free(s);
}

/* 扫描结果按信号强度降序 (强信号在前) */
static void scan_sort_by_rssi(void)
{
    int i, j;
    for (i = 1; i < s_scan.count; i++) {
        char key_s[33];
        int8_t key_r = s_scan.rssis[i];
        uint8_t key_c = s_scan.chans[i];
        strcpy(key_s, s_scan.ssids[i]);
        j = i - 1;
        while (j >= 0 && s_scan.rssis[j] < key_r) {
            strcpy(s_scan.ssids[j + 1], s_scan.ssids[j]);
            s_scan.chans[j + 1] = s_scan.chans[j];
            s_scan.rssis[j + 1] = s_scan.rssis[j];
            j--;
        }
        strcpy(s_scan.ssids[j + 1], key_s);
        s_scan.chans[j + 1] = key_c;
        s_scan.rssis[j + 1] = key_r;
    }
}

/* 配网状态查询 (手机端 save 后轮询):
 *   ok          配网成功 (GOT_IP, 即将重启)
 *   fail        连接失败 (密码错误等)
 *   connecting  正在连接中 */
static void handle_get_status(struct netconn *conn)
{
    cJSON *root = cJSON_CreateObject();
    if (s_configured) {
        cJSON_AddStringToObject(root, "status", "ok");
    } else if (s_conn_fail) {
        cJSON_AddStringToObject(root, "status", "fail");
    } else {
        cJSON_AddStringToObject(root, "status", "connecting");
    }
    reply_json(conn, root);
}

static void handle_get_network_list(struct netconn *conn)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int i;

    if (s_scanning) {
        /* 扫描进行中: 返回空数组, 手机端继续轮询 —— 否则只返回已保存的
         * WiFi 会让手机误以为扫描完成, 显示"只有原本连接的 WiFi" */
        cJSON_AddNumberToObject(root, "code", 1);
        cJSON_AddItemToObject(root, "data", arr);
        reply_json(conn, root);
        return;
    }
    scan_sort_by_rssi();    /* 按信号强度降序, 信号好的排前面 */

    for (i = 0; i < s_scan.count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s_scan.ssids[i]);
        cJSON_AddNumberToObject(o, "rssi", s_scan.rssis[i]);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "code", 1);
    cJSON_AddItemToObject(root, "data", arr);
    reply_json(conn, root);
}

/* POST /saveInfo: {"NEWORK":{"Network":ssid,"Password":pwd}[,"CITY":城市]} */
static void handle_save(struct netconn *conn, const char *body)
{
    cJSON *root = cJSON_Parse(body);
    const char *ssid = "", *pass = "", *city = NULL;
    char resp[160];

    if (!root) {
        send_raw(conn, hdr_badreq, sizeof(hdr_badreq) - 1);
        return;
    }
    cJSON *nwk = cJSON_GetObjectItem(root, "NEWORK");
    if (!nwk) {
        nwk = cJSON_GetObjectItem(root, "NETWORK");
    }
    if (nwk) {
        cJSON *jnet = cJSON_GetObjectItem(nwk, "Network");
        cJSON *jpw  = cJSON_GetObjectItem(nwk, "Password");
        if (jnet && (jnet->type & 0xFF) == cJSON_String && jnet->valuestring) {
            ssid = jnet->valuestring;
        }
        if (jpw && (jpw->type & 0xFF) == cJSON_String && jpw->valuestring) {
            pass = jpw->valuestring;
        }
    }
    cJSON *jcity = cJSON_GetObjectItem(root, "CITY");
    if (jcity && (jcity->type & 0xFF) == cJSON_String && jcity->valuestring
        && jcity->valuestring[0]) {
        city = jcity->valuestring;
    }
    cJSON_Delete(root);

    if (ssid[0] == 0) {
        send_raw(conn, hdr_badreq, sizeof(hdr_badreq) - 1);
        return;
    }
    printf("[PROV] saveInfo SSID=\"%s\" city=\"%s\"\r\n", ssid, city ? city : "-");

    /* 写 easyflash + 内存配置, 正常模式开机直接使用 */
    strncpy(g_cfg.ssid, ssid, sizeof(g_cfg.ssid) - 1);
    g_cfg.ssid[sizeof(g_cfg.ssid) - 1] = 0;
    strncpy(g_cfg.pwd, pass, sizeof(g_cfg.pwd) - 1);
    g_cfg.pwd[sizeof(g_cfg.pwd) - 1] = 0;
    if (city) {
        strncpy(g_cfg.city, city, sizeof(g_cfg.city) - 1);
        g_cfg.city[sizeof(g_cfg.city) - 1] = 0;
    }
    if (cfg_save(&g_cfg) == 0) {
        s_connecting = 1;
        s_conn_fail = 0;                    /* 重新开始连接: 清除上次失败状态 */
        s_retry_left = PROV_RETRY_MAX;      /* 重置自动重试计数 */
        wifi_mgmr_sta_autoconnect_enable(); /* 恢复 (上次连接失败时已关闭) */
        prov_ui_status("已收到! 正在连接 WiFi, 即将重启", C_ACC);
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\"}");
        /* 必须先发响应再启 STA 连接: 连接过程 WiFi 固件忙于关联路由器,
         * AP 数据通路会中断, 响应后置会丢失 → 手机一直"保存中"然后超时 */
        send_raw(conn, hdr_json, sizeof(hdr_json) - 1);
        send_raw(conn, resp, strlen(resp));

        /* 单射频 (AP+STA 共用一块射频): AP 与 STA 必须同信道, 否则路由器在
         * DHCP 开始的瞬间就把 STA 踢掉 (日志: Deauth/Disassociate by AP)。
         * 先扫到目标 WiFi 所在信道, 把 AP 切过去再连 STA —— 同信道时 (如
         * FAE@Seahi 恰好在 AP 原 ch6) 一次成功, 跨信道必被踢。
         * CSA 切换时手机自动跟随到新信道 */
        {
            int tch = prov_scan_match_channel(g_cfg.ssid);
            if (tch >= 1 && tch <= 13) {
                s_try_chan = tch;   /* 记录目标信道: 重试任务也定向连接 */
                if (tch != s_ap_chan) {
                    wifi_mgmr_ap_chan_switch(&s_ap_if, tch, 0);
                    /* 切换是异步事件 (mgmr 事件队列) + 固件发 CSA beacon 后
                     * 才真正换信道, 手机跟随也要时间: 等 1s 再启 STA。
                     * 若等不够, STA 已在新信道而 AP 还在旧信道 → 单射频冲突
                     * → 路由器在 DHCP 阶段踢 STA (sc=6) */
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    s_ap_chan = tch;
                    printf("[PROV] AP chan -> %d (match %s)\r\n", tch, g_cfg.ssid);
                } else {
                    printf("[PROV] target on ch%d, AP already there\r\n", tch);
                }
            } else {
                s_try_chan = 0;     /* 信道未知: 保持 AP 现状, STA 全信道扫描 */
                printf("[PROV] target ch unknown, keep AP ch%d\r\n", s_ap_chan);
            }
        }
        wifi_interface_t iface = wifi_mgmr_sta_enable();
        /* chan_id = s_try_chan: 已知信道定向连接, 免全信道扫描 (~4s, 期间射频
         * 逐信道跳频会打断 AP 数据通路, 且拉长 AP/STA 异信道冲突窗口) */
        wifi_mgmr_sta_connect(iface, g_cfg.ssid, g_cfg.pwd, NULL, NULL, 0, s_try_chan);
    } else {
        snprintf(resp, sizeof(resp), "{\"status\":\"err\",\"error\":\"flash save failed\"}");
        prov_ui_status("配网失败, 请重连", C_ERR);
        send_raw(conn, hdr_json, sizeof(hdr_json) - 1);
        send_raw(conn, resp, strlen(resp));
    }
}

/* 读取一个完整 HTTP 请求, 返回 body 偏移或 -1 */
static int recv_http_request(struct netconn *conn, char *buf, size_t buf_sz)
{
    struct netbuf *inbuf = NULL;
    size_t total = 0;
    err_t err;

    for (;;) {
        err = netconn_recv(conn, &inbuf);
        if (err != ERR_OK || inbuf == NULL) {
            return -1;
        }
        char *data;
        u16_t len;
        netbuf_data(inbuf, (void **)&data, &len);
        if (total + len >= buf_sz - 1) {
            netbuf_delete(inbuf);
            return -1;
        }
        memcpy(buf + total, data, len);
        total += len;
        buf[total] = '\0';
        netbuf_delete(inbuf);
        inbuf = NULL;

        char *hend = strstr(buf, "\r\n\r\n");
        if (!hend) {
            continue;
        }
        if (strncasecmp(buf, "POST ", 5) == 0) {
            /* 非 /saveInfo 的 POST (微信 mmtls 等后台 App TLS 探测被 DNS 劫持到
             * 这里) 不是合法 HTTP 请求: 不等待 body, 立即放行由 handle_client
             * 404 关闭 —— 否则等 body 会卡住服务器最多 15 秒, 用户点"连接"的
             * /saveInfo 排队, 表现为"传密码要等十几秒" */
            if (strncmp(buf + 5, "/saveInfo", 9) != 0) {
                return (int)(hend + 4 - buf);
            }
            /* 简单解析 content-length */
            int bodylen = -1;
            char *line = buf;
            while (line < hend) {
                char *eol = strstr(line, "\r\n");
                if (!eol || eol > hend) {
                    break;
                }
                if (strncasecmp(line, "content-length:", 15) == 0) {
                    bodylen = atoi(line + 15);
                    break;
                }
                line = eol + 2;
            }
            size_t hlen = (size_t)(hend + 4 - buf);
            if (bodylen <= 0 || hlen + (size_t)bodylen > buf_sz - 1) {
                return -1;
            }
            if (total < hlen + (size_t)bodylen) {
                continue;
            }
        }
        return (int)(hend + 4 - buf);
    }
}

static void handle_client(struct netconn *conn, char *buf)
{
    char method[8] = {0};
    char path[256] = {0};
    int body_off;

    body_off = recv_http_request(conn, buf, REQ_BUF_SZ);
    if (body_off < 0) {
        if (strncasecmp(buf, "POST ", 5) == 0) {
            send_raw(conn, hdr_badreq, sizeof(hdr_badreq) - 1);
        }
        netconn_close(conn);
        return;
    }
    if (sscanf(buf, "%7s %255s", method, path) < 2) {
        send_raw(conn, hdr_404, sizeof(hdr_404) - 1);
        netconn_close(conn);
        return;
    }
    /* 先归一化 absolute-form (http://host/path), 再去 query */
    if (path[0] != '/') {
        char *slash = NULL;
        if (strncasecmp(path, "http://", 7) == 0) {
            slash = strchr(path + 7, '/');
            if (!slash || slash[1] == '\0') {
                strcpy(path, "/");
            } else {
                memmove(path, slash, strlen(slash) + 1);
            }
        } else if (strncasecmp(path, "https://", 8) == 0) {
            slash = strchr(path + 8, '/');
            if (slash && slash[1]) {
                memmove(path, slash, strlen(slash) + 1);
            } else {
                strcpy(path, "/");
            }
        }
    }
    {
        char *q = strchr(path, '?');
        if (q) {
            *q = '\0';
        }
    }
    printf("[PROV] HTTP %s %s\r\n", method, path);

    if (strcasecmp(method, "OPTIONS") == 0) {
        /* CORS 预检: 响应放行, 避免浏览器拦截 */
        send_raw(conn, hdr_cors, sizeof(hdr_cors) - 1);
        netconn_close(conn);
        return;
    }
    if (strcasecmp(method, "GET") == 0 && strcmp(path, "/favicon.ico") == 0) {
        send_raw(conn, hdr_204, sizeof(hdr_204) - 1);   /* 页面已内联 favicon */
        netconn_close(conn);
        return;
    }
    if (strcasecmp(method, "GET") == 0) {
        if (strcmp(path, "/scan") == 0) {
            prov_scan_refresh();
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "status", "scanning");
            reply_json(conn, o);
        } else if (strcmp(path, "/getNetWork") == 0) {
            handle_get_network_list(conn);
        } else if (strcmp(path, "/getStatus") == 0) {
            handle_get_status(conn);
        } else if (path_is_captive_probe(path)) {
            if (s_configured) {
                /* 配网成功: 探活路径返回 204 → 手机系统判定"网络已通",
                 * 自动关闭 captive portal 配网页 (不等 WiFi 断开) */
                send_raw(conn, hdr_204, sizeof(hdr_204) - 1);
            } else {
                send_raw(conn, hdr_html, sizeof(hdr_html) - 1);
                prov_send_page(conn);
            }
        } else {
            send_raw(conn, hdr_404, sizeof(hdr_404) - 1);
        }
    } else if (strcasecmp(method, "POST") == 0 && strcmp(path, "/saveInfo") == 0) {
        handle_save(conn, buf + body_off);
    } else {
        send_raw(conn, hdr_404, sizeof(hdr_404) - 1);
    }
    netconn_close(conn);
}

/* HTTP 服务器: 单 worker + 编译期静态缓冲 (BSS 不占堆, BL602 堆紧张)。
 * 80 端口同时服务探活洪流与配网交互: 慢连接靠 2 秒 recvtimeout 兜底,
 * 非 /saveInfo 的 POST (mmtls 等) 立即拒绝 —— 探活每个 <10ms 处理完,
 * saveInfo 排队最坏 ~2 秒, 远好于早期 mmtls 卡服务器 15 秒的问题 */
static struct netconn *s_listen80;

static void http_server_worker(void *arg)
{
    struct netconn *conn = NULL;
    static char buf[REQ_BUF_SZ];    /* 单 worker 单缓冲, 无竞争 */

    (void)arg;
    for (;;) {
        if (netconn_accept(s_listen80, &conn) == ERR_OK && conn) {
            netconn_set_recvtimeout(conn, 2000);    /* 任何连接最多占 2 秒 */
            handle_client(conn, buf);
            netconn_delete(conn);
            conn = NULL;
        }
    }
}

void prov_http_start(void)
{
    if (s_http_started) {
        return;
    }
    s_http_started = 1;
    s_listen80 = netconn_new(NETCONN_TCP);
    if (s_listen80) {
        netconn_bind(s_listen80, NULL, 80);
        netconn_listen(s_listen80);
    }
    xTaskCreate(http_server_worker, "prov_http", 2048, NULL, 12, NULL);
}

/* 配网 UI 初始化: main() 在 LVGL 就绪后调用 (创建界面并切换到配网屏)。
 * 幂等: conn_timeout_task(45s) 可能在 boot 启动屏等待期间已触发 prov_enter
 * (内部已调用本函数切屏), main 分支再次调用时直接跳过, 避免重复创建 */
void prov_ui_init(void)
{
    xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
    if (lv_scr_act() != p_scr) {
        prov_ui_create();
        lv_scr_load(p_scr);
    }
    xSemaphoreGive(lvgl_mutex);
    /* 占位符 ????: 界面一上屏就补真实 AP 名(读 MAC 不依赖 WiFi 固件,
     * 失败兜底 CONF, MGMR_DONE 后由 prov_gen_ap_ssid 再更新为真实名) */
    prov_gen_ap_ssid();
}

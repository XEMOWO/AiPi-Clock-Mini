/**
 * @file weather.c — 天气轮询任务: 高德(HTTP) / 和风(HTTPS+gzip) 双源
 *
 * 天气源由配置 g_cfg.wx_api 决定, 串口 #XWXAPI 可切换, API Key 由
 * #XWXKEY 下发(见 xcmd.c)。和风实时天气:
 *   geoapi.qweather.com/v2/city/lookup  -> LocationID (城市/高德adcode 均可)
 *   devapi.qweather.com/v7/weather/now  -> 实时天气(强制 gzip, 需解压)
 * 认证用 X-QW-Api-Key 请求头 (旧 key=凭据ID 方式已失效)。
 * HTTPS 走 SDK 预构建 mbedtls_lts, 无 CA 证书存储 -> VERIFY_NONE。
 */
#include <stdio.h>
#include <string.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/debug.h"
#include "cJSON.h"
#include "gui_guider.h"
#include "wx_icons.h"
#include "custom.h"     /* wx_full_name / weather_task */
#include "gz.h"
#include "cfg_store.h"


#define WX_BUF 2048             /* HTTP 响应 / 解压后 JSON 缓冲 */

static uint8_t s_http[WX_BUF];
static uint8_t s_json[WX_BUF];

/* 和风 LocationID 缓存: 城市变了才重新反查 */
static char s_loc_city[16];
static char s_loc_id[16];

/* ===== 小工具 ===== */

/* URL 编码(UTF-8 字节流): 中文城市名 -> %XX */
static void url_encode(const char *src, char *dst, int cap)
{
    static const char hex[] = "0123456789ABCDEF";

    while (*src && cap > 4) {
        uint8_t c = (uint8_t)*src++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *dst++ = (char)c;
            cap--;
        } else {
            *dst++ = '%';
            *dst++ = hex[c >> 4];
            *dst++ = hex[c & 15];
            cap -= 3;
        }
    }
    *dst = 0;
}

/* 不区分大小写的子串查找 */
static const char *ci_strstr(const char *hay, int haylen, const char *needle)
{
    size_t nl = strlen(needle);

    for (int i = 0; i + (int)nl <= haylen; i++) {
        int j;
        for (j = 0; j < (int)nl; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == (int)nl) return hay + i;
    }
    return NULL;
}

/*
 * 解析 HTTP 响应: 校验状态行, 提取 body; Content-Encoding: gzip 时
 * 用 gz_inflate 解压到 out。返回 body 指针, *outlen=body 长度, 出错 NULL。
 */
static const char *http_body(const uint8_t *buf, int len,
                             uint8_t *out, int outcap, int *outlen)
{
    const char *hdr_end = ci_strstr((const char *)buf, len, "\r\n\r\n");
    const char *cl_hdr;
    const char *body;
    int body_off, clen = -1, gzip = 0;

    if (!hdr_end || len < 12) return NULL;
    if (memcmp(buf, "HTTP/1.", 7) != 0) return NULL;
    if (!(buf[9] == '2' && buf[10] == '0' && buf[11] == '0')) return NULL; /* 只认 2xx */

    cl_hdr = ci_strstr((const char *)buf, (int)(hdr_end - (const char *)buf),
                       "content-length:");
    if (cl_hdr) clen = atoi(cl_hdr + 15);
    if (ci_strstr((const char *)buf, (int)(hdr_end - (const char *)buf),
                  "content-encoding: gzip")) gzip = 1;

    body_off = (int)(hdr_end - (const char *)buf) + 4;
    body = (const char *)buf + body_off;
    if (gzip) {
        int r = gz_inflate((const uint8_t *)body, len - body_off, out, outcap);
        if (r < 0) return NULL;
        *outlen = r;
        return (const char *)out;
    }
    if (clen < 0) clen = len - body_off;
    if (clen > len - body_off) clen = len - body_off;
    if (clen > outcap) return NULL;
    memcpy(out, body, clen);
    *outlen = clen;
    return (const char *)out;
}

/* ===== HTTP (高德) ===== */

static int http_get(const char *host, uint16_t port, const char *req,
                    uint8_t *buf, int cap)
{
    struct hostent *h = gethostbyname(host);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    struct timeval tv = { 10, 0 };
    int s, t = 0;

    if (!h) return -1;
    memcpy(&a.sin_addr, h->h_addr, h->h_length);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }
    if (write(s, req, strlen(req)) <= 0) { close(s); return -1; }
    while (t < cap - 1) {
        int r = (int)read(s, buf + t, cap - 1 - t);
        if (r <= 0) break;
        t += r;
        /* header + Content-Length 齐了可提前停, 不等对端 close */
        const char *hd = ci_strstr((const char *)buf, t, "\r\n\r\n");
        if (hd) {
            const char *cl = ci_strstr((const char *)buf,
                                       (int)(hd - (const char *)buf),
                                       "content-length:");
            if (cl && t >= atoi(cl + 15) + (int)(hd - (const char *)buf) + 4) break;
        }
    }
    close(s);
    return t;
}

/* ===== 城市搜索(高德 geocode): 城市名 → adcode 候选列表 =====
 * 返回静态缓冲 "名称:adcode;名称:adcode;...", *n=候选数(截断到 maxn);
 * 查询失败(无key/网络/JSON/响应过大)返回 NULL。
 * 独立静态缓冲 + 静态输出, 与 weather_task 的 s_http/s_json 互不干扰,
 * 可在串口回调上下文直接调用。 */
static uint8_t s_chttp[2048];
static uint8_t s_cjson[2048];
static char s_city_out[320];

const char *wx_city_search(const char *name, int *n, int maxn)
{
    char req[288], enc[64];
    int len, cnt = -1;
    cJSON *root = NULL;
    cJSON *gc;

    *n = 0;
    if (!g_cfg.amap_key[0] || strlen(name) > 40) return NULL;
    url_encode(name, enc, sizeof(enc));
    snprintf(req, sizeof(req),
        "GET /v3/geocode/geo?address=%s&key=%s HTTP/1.1\r\n"
        "Host: restapi.amap.com\r\nConnection: close\r\n\r\n",
        enc, g_cfg.amap_key);
    len = http_get("restapi.amap.com", 80, req, s_chttp, sizeof(s_chttp));
    if (len <= 0) return NULL;
    const char *body = http_body(s_chttp, len, s_cjson, sizeof(s_cjson), &len);
    if (!body) return NULL;                              /* 响应过大或非 2xx */
    s_cjson[len] = 0;
    root = cJSON_Parse(body);
    if (!root) return NULL;
    gc = cJSON_GetObjectItem(root, "geocodes");
    cnt = gc ? cJSON_GetArraySize(gc) : 0;
    if (cnt > maxn) cnt = maxn;
    s_city_out[0] = 0;
    for (int i = 0; i < cnt; i++) {
        cJSON *it = cJSON_GetArrayItem(gc, i);
        cJSON *fa = cJSON_GetObjectItem(it, "formatted_address");
        cJSON *ad = cJSON_GetObjectItem(it, "adcode");
        size_t used = strlen(s_city_out);
        if (used >= sizeof(s_city_out) - 48) break;
        snprintf(s_city_out + used, sizeof(s_city_out) - used,
                 "%s:%s;", fa && fa->valuestring ? fa->valuestring : "?",
                 ad && ad->valuestring ? ad->valuestring : "?");
    }
    cJSON_Delete(root);
    *n = cnt;
    return s_city_out;
}

/* ===== HTTPS (和风, mbedtls_lts) ===== */

/* mbedtls 调试回调: 打印握手过程, 用于定位 BAD_INPUT_DATA 失败点 */
static void wx_mbedtls_debug(void *ctx, int level,
                             const char *file, int line, const char *str)
{
    (void)ctx; (void)level;
    printf("[WX] %s:%04d %s", file, line, str);
}

static int https_get(const char *host, const char *req, uint8_t *buf, int cap)
{
    /* mbedtls 上下文较大(entropy~500B/ctr_drbg/ssl 各数百B), 任务栈只有 4KB,
     * 改到 aos 堆分配, 栈上只留指针 */
    mbedtls_net_context *net = malloc(sizeof(mbedtls_net_context));
    mbedtls_ssl_context *ssl = malloc(sizeof(mbedtls_ssl_context));
    mbedtls_ssl_config *conf = malloc(sizeof(mbedtls_ssl_config));
    mbedtls_entropy_context *entropy = malloc(sizeof(mbedtls_entropy_context));
    mbedtls_ctr_drbg_context *ctr = malloc(sizeof(mbedtls_ctr_drbg_context));
    int ret = -1, t = 0;

    if (!net || !ssl || !conf || !entropy || !ctr) {
        printf("[WX] https %s malloc fail\r\n", host);
        goto fail;
    }
    mbedtls_net_init(net);
    mbedtls_ssl_init(ssl);
    mbedtls_ssl_config_init(conf);
    mbedtls_entropy_init(entropy);
    mbedtls_ctr_drbg_init(ctr);

    if (mbedtls_ctr_drbg_seed(ctr, mbedtls_entropy_func, entropy, NULL, 0) != 0)
        goto out;
    if (mbedtls_net_connect(net, host, "443", MBEDTLS_NET_PROTO_TCP) != 0)
        goto out;
    if (mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        goto out;
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE); /* 无 CA 证书存储 */
    mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, ctr);
    mbedtls_ssl_conf_read_timeout(conf, 15000);
    mbedtls_ssl_conf_dbg(conf, wx_mbedtls_debug, NULL);
    mbedtls_debug_set_threshold(2);
    if (mbedtls_ssl_setup(ssl, conf) != 0) goto out;
    mbedtls_ssl_set_hostname(ssl, host);    /* SNI */
    mbedtls_ssl_set_bio(ssl, net, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);
    do {
        ret = mbedtls_ssl_handshake(ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret != 0) goto out;

    if (mbedtls_ssl_write(ssl, (const unsigned char *)req, strlen(req)) <= 0)
        goto out;
    for (;;) {
        int r = mbedtls_ssl_read(ssl, buf + t, cap - 1 - t);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) break;
        if (r < 0) { ret = -1; goto out; }
        t += r;
        const char *hd = ci_strstr((const char *)buf, t, "\r\n\r\n");
        if (hd) {
            const char *cl = ci_strstr((const char *)buf,
                                       (int)(hd - (const char *)buf),
                                       "content-length:");
            if (cl && t >= atoi(cl + 15) + (int)(hd - (const char *)buf) + 4) break;
        }
        if (t >= cap - 1) break;
    }
    ret = t;
out:
    mbedtls_ssl_close_notify(ssl);
    mbedtls_ssl_free(ssl);
    mbedtls_ssl_config_free(conf);
    mbedtls_net_close(net);
    mbedtls_net_free(net);
    mbedtls_entropy_free(entropy);
    mbedtls_ctr_drbg_free(ctr);
fail:
    free(net); free(ssl); free(conf); free(entropy); free(ctr);
    if (ret < 0) printf("[WX] https %s fail=%d\r\n", host, ret);
    return ret;
}

/* ===== UI 更新(共享路径) ===== */

static void ui_wx_update(const char *wx, const char *temp, const char *humi)
{
    /* 新 UI(TEST3)无天气控件: 只保留数据日志, 界面更新已禁用 */
    (void)wx;
    printf("[WX] wendu=[%s] shidu=[%s]\r\n", temp, humi);
}

/* ===== 高德: lives[0].{weather,temperature,humidity} ===== */

static int wx_fetch_amap(void)
{
    char req[288], enc[64];
    int len;

    url_encode(g_cfg.city, enc, sizeof(enc));
    snprintf(req, sizeof(req),
        "GET /v3/weather/weatherInfo?city=%s&key=%s HTTP/1.1\r\n"
        "Host: restapi.amap.com\r\nConnection: close\r\n\r\n",
        enc, g_cfg.amap_key);
    len = http_get("restapi.amap.com", 80, req, s_http, sizeof(s_http));
    if (len <= 0) { printf("[WX] amap http fail=%d\r\n", len); return -1; }
    const char *body = http_body(s_http, len, s_json, sizeof(s_json), &len);
    if (!body) { printf("[WX] amap parse fail len=%d\r\n", len); return -1; }
    cJSON *root = cJSON_Parse(body);
    int ok = -1;
    if (root) {
        cJSON *lives = cJSON_GetObjectItem(root, "lives");
        cJSON *it = lives ? cJSON_GetArrayItem(lives, 0) : NULL;
        if (it) {
            cJSON *w = cJSON_GetObjectItem(it, "weather");
            cJSON *temp = cJSON_GetObjectItem(it, "temperature");
            cJSON *humi = cJSON_GetObjectItem(it, "humidity");
            ui_wx_update(w ? w->valuestring : "?",
                         temp ? temp->valuestring : "?",
                         humi ? humi->valuestring : "");
            ok = 0;
        }
        cJSON_Delete(root);
    }
    return ok;
}

/* ===== Open-Meteo: 免费无 key, 明文 HTTP (绕开 mbedtls 内存问题)
 * 1) geocoding 反查城市经纬度 → 2) forecast 查实时天气 (WMO code → 中文/图标) ===== */
static char s_om_lat[12] = "";
static char s_om_lon[12] = "";

static int wx_om_geo(void)
{
    char req[256], enc[64];
    int len;

    url_encode(g_cfg.city, enc, sizeof(enc));
    /* 用 HTTP/1.0: Open-Meteo 对 1.1 返回 chunked 编码(解析器不支持),
     * 1.0 下强制 Connection: close 结尾, body 为纯 JSON */
    snprintf(req, sizeof(req),
        "GET /v1/search?name=%s&count=1&language=zh&format=json HTTP/1.0\r\n"
        "Host: geocoding-api.open-meteo.com\r\nConnection: close\r\n\r\n", enc);
    len = http_get("geocoding-api.open-meteo.com", 80, req, s_http, sizeof(s_http));
    if (len <= 0) { printf("[WX] om geo http fail=%d\r\n", len); return -1; }
    const char *body = http_body(s_http, len, s_json, sizeof(s_json), &len);
    if (!body) { printf("[WX] om geo parse fail len=%d\r\n", len); return -1; }
    s_json[len] = 0;                       /* cJSON_Parse 需要 NUL 结尾 */
    cJSON *root = cJSON_Parse(body);
    int ok = -1;
    if (root) {
        cJSON *res = cJSON_GetObjectItem(root, "results");
        cJSON *it = res ? cJSON_GetArrayItem(res, 0) : NULL;
        if (it) {
            cJSON *la = cJSON_GetObjectItem(it, "latitude");
            cJSON *lo = cJSON_GetObjectItem(it, "longitude");
            if (la && lo && la->type == cJSON_Number && lo->type == cJSON_Number) {
                snprintf(s_om_lat, sizeof(s_om_lat), "%.4f", la->valuedouble);
                snprintf(s_om_lon, sizeof(s_om_lon), "%.4f", lo->valuedouble);
                printf("[WX] om geo %s -> %s,%s\r\n", g_cfg.city, s_om_lat, s_om_lon);
                ok = 0;
            }
        }
        cJSON_Delete(root);
        if (ok != 0) {   /* 查无结果: 多为城市名是数字 adcode, Open-Meteo 不认 */
            printf("[WX] om geo no result city=[%s] body=%.80s\r\n",
                   g_cfg.city, body);
        }
    }
    return ok;
}

/* WMO weather code → 中文天气词(UI 显示 + wx_get_icon 图标匹配) */
static const char *wx_om_text(int code)
{
    if (code == 0) return "晴";
    if (code <= 3) return "多云";
    if (code <= 48) return "雾";
    if (code <= 57) return "小雨";
    if (code <= 67) return "小雨";
    if (code <= 77) return "雪";
    if (code <= 82) return "阵雨";
    if (code <= 86) return "雪";
    return "雷阵雨";
}

static int wx_fetch_openmeteo(void)
{
    char req[288];
    int len;

    /* 用 HTTP/1.0: Open-Meteo 对 1.1 返回 chunked 编码(解析器不支持),
     * 1.0 下强制 Connection: close 结尾, body 为纯 JSON */
    snprintf(req, sizeof(req),
        "GET /v1/forecast?latitude=%s&longitude=%s"
        "&current=temperature_2m,relative_humidity_2m,weather_code HTTP/1.0\r\n"
        "Host: api.open-meteo.com\r\nConnection: close\r\n\r\n",
        s_om_lat, s_om_lon);
    len = http_get("api.open-meteo.com", 80, req, s_http, sizeof(s_http));
    if (len <= 0) { printf("[WX] om http fail=%d\r\n", len); return -1; }
    const char *body = http_body(s_http, len, s_json, sizeof(s_json), &len);
    if (!body) { printf("[WX] om parse fail len=%d\r\n", len); return -1; }
    s_json[len] = 0;                       /* cJSON_Parse 需要 NUL 结尾 */
    cJSON *root = cJSON_Parse(body);
    int ok = -1;
    if (root) {
        cJSON *cur = cJSON_GetObjectItem(root, "current");
        if (cur) {
            cJSON *t = cJSON_GetObjectItem(cur, "temperature_2m");
            cJSON *h = cJSON_GetObjectItem(cur, "relative_humidity_2m");
            cJSON *w = cJSON_GetObjectItem(cur, "weather_code");
            int code = (w && w->type == cJSON_Number) ? (int)w->valuedouble : 0;
            char temp[16], humi[16];
            if (t && t->type == cJSON_Number)
                snprintf(temp, sizeof(temp), "%.0f", t->valuedouble);
            else
                strcpy(temp, "?");
            if (h && h->type == cJSON_Number)
                snprintf(humi, sizeof(humi), "%.0f", h->valuedouble);
            else
                strcpy(humi, "");
            printf("[WX] om parsed text=[%s] temp=[%s] humi=[%s]\r\n",
                   wx_om_text(code), temp, humi);
            ui_wx_update(wx_om_text(code), temp, humi);
            ok = 0;
        } else {
            printf("[WX] om no current field: %.120s\r\n", body);
        }
        cJSON_Delete(root);
    } else {
        printf("[WX] om json fail: %.120s\r\n", body);
    }
    return ok;
}

/* ===== 和风: geoapi 反查 LocationID (城市/adcode 均可, 返回 200 才算成功) ===== */

static int wx_qw_geo(void)
{
    char req[288], enc[64];
    int len;

    url_encode(g_cfg.city, enc, sizeof(enc));
    snprintf(req, sizeof(req),
        "GET /v2/city/lookup?location=%s&range=cn HTTP/1.1\r\n"
        "Host: geoapi.qweather.com\r\n"
        "X-QW-Api-Key: %s\r\nConnection: close\r\n\r\n",
        enc, g_cfg.qw_key);
    len = https_get("geoapi.qweather.com", req, s_http, sizeof(s_http));
    if (len <= 0) { printf("[WX] qw geo https fail=%d\r\n", len); return -1; }
    const char *body = http_body(s_http, len, s_json, sizeof(s_json), &len);
    if (!body) { printf("[WX] qw geo parse fail len=%d\r\n", len); return -1; }
    cJSON *root = cJSON_Parse(body);
    int ok = -1;
    if (root) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *loc = cJSON_GetObjectItem(root, "location");
        if (code && strcmp(code->valuestring, "200") == 0 &&
            loc && cJSON_GetArrayItem(loc, 0)) {
            cJSON *id = cJSON_GetObjectItem(cJSON_GetArrayItem(loc, 0), "id");
            if (id && id->valuestring) {
                snprintf(s_loc_id, sizeof(s_loc_id), "%s", id->valuestring);
                ok = 0;
            }
        } else {
            printf("[WX] qw geo code=%s\r\n",
                   code && code->valuestring ? code->valuestring : "none");
        }
        cJSON_Delete(root);
    } else {
        printf("[WX] qw geo json fail\r\n");
    }
    return ok;
}

/* ===== 和风: now.{text,temp,humidity} (湿度补 % 号) ===== */

static int wx_fetch_qweather(void)
{
    char req[288];
    int len;

    snprintf(req, sizeof(req),
        "GET /v7/weather/now?location=%s HTTP/1.1\r\n"
        "Host: devapi.qweather.com\r\n"
        "X-QW-Api-Key: %s\r\nConnection: close\r\n\r\n",
        s_loc_id, g_cfg.qw_key);
    len = https_get("devapi.qweather.com", req, s_http, sizeof(s_http));
    if (len <= 0) { printf("[WX] qw now https fail=%d\r\n", len); return -1; }
    const char *body = http_body(s_http, len, s_json, sizeof(s_json), &len);
    if (!body) { printf("[WX] qw now parse fail len=%d\r\n", len); return -1; }
    s_json[len] = 0;
    printf("[WX] qw body(%d) heap=%u: %s\r\n", len,
           (unsigned)xPortGetFreeHeapSize(), body);
    cJSON *root = cJSON_Parse(body);
    int ok = -1;
    if (root) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *now = cJSON_GetObjectItem(root, "now");
        if (code && strcmp(code->valuestring, "200") == 0 && now) {
            cJSON *h = cJSON_GetObjectItem(now, "humidity");
            {
                cJSON *t = cJSON_GetObjectItem(now, "temp");
                cJSON *x = cJSON_GetObjectItem(now, "text");
                printf("[WX] qw parsed text=[%s] temp=[%s] humi=[%s] heap=%u\r\n",
                       x && x->valuestring ? x->valuestring : "(null)",
                       t && t->valuestring ? t->valuestring : "(null)",
                       h && h->valuestring ? h->valuestring : "(null)",
                       (unsigned)xPortGetFreeHeapSize());
            }
            ui_wx_update(
                cJSON_GetObjectItem(now, "text") ? cJSON_GetObjectItem(now, "text")->valuestring : "?",
                cJSON_GetObjectItem(now, "temp") ? cJSON_GetObjectItem(now, "temp")->valuestring : "?",
                h && h->valuestring ? h->valuestring : "");
            ok = 0;
        } else {
            printf("[WX] qw now code=%s\r\n",
                   code && code->valuestring ? code->valuestring : "none");
        }
        cJSON_Delete(root);
    } else {
        printf("[WX] qw now json fail\r\n");
    }
    return ok;
}

/* ===== 轮询任务: 60s/次, 配置被串口修改后立即重查 ===== */

void weather_task(void *pv)
{
    int qw_fail = 0;    /* 和风连续失败次数: >=2 时临时降级高德, 保证天气有显示 */

    printf("[WX] task start api=%s\r\n", g_cfg.wx_api);
    vTaskDelay(pdMS_TO_TICKS(3000));
    for (;;) {
        if (g_cfg_dirty) { g_cfg_dirty = 0; qw_fail = 0; } /* 配置被改过, 重新查询 */

        if (strcmp(g_cfg.wx_api, "qweather") == 0) {
            int ok = -1;

            if (strcmp(s_loc_city, g_cfg.city) != 0) {
                s_loc_city[0] = 0;          /* 城市变了, 反查新的 LocationID */
                if (wx_qw_geo() == 0)
                    strcpy(s_loc_city, g_cfg.city);
            }
            if (s_loc_city[0])
                ok = wx_fetch_qweather();
            else
                printf("[WX] qw skip (no loc id)\r\n");

            if (ok == 0) {
                qw_fail = 0;
            } else if (++qw_fail >= 2) {
                /* 和风连败(多为 TLS 内存不足): 降级高德 HTTP (lwip 静态池, 不占
                 * FreeRTOS 堆), 60s 后仍会再试和风 */
                printf("[WX] qw fail %d 次, 降级高德\r\n", qw_fail);
                wx_fetch_amap();
            }
        } else if (strcmp(g_cfg.wx_api, "openmeteo") == 0) {
            if (strcmp(s_loc_city, g_cfg.city) != 0) {
                s_loc_city[0] = 0;          /* 城市变了, 重新反查经纬度 */
                if (wx_om_geo() == 0)
                    strcpy(s_loc_city, g_cfg.city);
            }
            if (s_loc_city[0])
                wx_fetch_openmeteo();
            else
                printf("[WX] om skip (no loc)\r\n");
        } else {
            wx_fetch_amap();
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

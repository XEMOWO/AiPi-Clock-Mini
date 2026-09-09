/**
 * @brief cfg_store — easyflash 持久化封装（WiFi/城市配置）
 */
#ifndef CFG_STORE_H
#define CFG_STORE_H

typedef struct {
    char ssid[33];  /* wifi 栈硬限 32 字节 */
    char pwd[65];
    char city[16];  /* 城市: 高德 adcode(如 440306)或中文名, 和风走 geoapi 反查 */
    char pc_ip[16]; /* Claude Code 监控: PC 局域网 IP */
    uint8_t mon_on; /* Claude Code 监控开关 0/1 */
    uint8_t wx_on;  /* 天气功能开关: 0=关闭(默认, 代码保留) 1=开启 */
    char wx_api[16];   /* 天气源: "amap"=高德 | "qweather"=和风 */
    char amap_key[33]; /* 高德 Web服务 key */
    char qw_key[33];   /* 和风 API Key (X-QW-Api-Key 头) */
    char qw_cred[33];  /* 和风凭据ID (旧版认证用, 当前认证方式未使用) */
    uint8_t clock_anim; /* 时钟数字样式: 1=滚动翻页动画 0=直接切换(无动画) */
    uint8_t wall_valid; /* 壁纸有效标记: media 分区已上传过完整图片 */
    uint8_t wall_mode;  /* 图播放: 1=时钟/壁纸按各自秒数交替 0=关(始终时钟) */
    uint8_t wall_sec;   /* 图播放: 壁纸显示秒数 1-60, 默认 5 */
    uint8_t wall_clock_sec; /* 图播放: 时钟显示秒数 1-60, 默认 5 */
    uint8_t wall_anim;  /* 壁纸/时钟切换过渡动画: 0=无 1=淡入 2=左滑 3=右滑 4=上滑 5=缩放 6=随机 */
    uint32_t bg_c1;     /* 时钟屏背景渐变起色 0xRRGGBB, 默认淡蓝 0xCDE6F8 */
    uint32_t bg_c2;     /* 时钟屏背景渐变止色 0xRRGGBB, 默认淡紫 0xE9DCF6 */
    uint8_t bg_dir;     /* 背景样式: 0=纯色(用 c1) 1=水平渐变 2=垂直渐变 */
} cfg_t;

/* 全局配置,由 cfg_store.c 定义,main.c / xcmd.c 共用 */
extern cfg_t g_cfg;

/* 配置被串口命令修改后置 1,weather_task 检测到会立即重查天气 */
extern volatile uint8_t g_cfg_dirty;

/* Claude Code 状态灯: 0=呼吸 1/2=黄 3=绿 4=红, -1=无效 */
extern volatile int8_t g_lamp_status;

void cfg_load_default(cfg_t *c);   /* 填充编译期默认值 */
int  cfg_load(cfg_t *c);           /* 从 flash 读,无则用默认值 */
int  cfg_save(const cfg_t *c);     /* 写回 flash,0=成功 */

#endif /* CFG_STORE_H */

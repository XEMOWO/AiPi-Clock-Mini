/**
 * @brief AiPi-Clock-Mini — ST7789V 172x320 + LVGL + WiFi + SNTP 时间
 */
#include <stdio.h>
#include <string.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include "bl_sys.h"
#include "lvgl.h"
#include <hosal_gpio.h>
#include <hosal_timer.h>
#include <bl602.h>
#include <bl602_spi.h>
#include <bl602_glb.h>
#include <bl602_gpio.h>
#include <aos/yloop.h>
#include <aos/kernel.h>
#include <lwip/tcpip.h>
#include <lwip/dns.h>
#include <lwip/ip4_addr.h>
#include <wifi_mgmr_ext.h>
#include <hal_wifi.h>
#include <sntp.h>
#include <utils_time.h>
#include <easyflash.h>
#include <bl_mtd.h>
#include <bl_sys_time.h>   /* 系统毫秒时间 (sntp 同步时写入, FreeRTOS tick 推进) */
#include "bl_wdt.h"     /* 硬件看门狗(死机自动复位) */

#include "gui_guider.h"
#include "gg_utils.h"
#include "custom.h"
#include "xcmd.h"
#include "cfg_store.h"
#include "img_upload.h" /* 壁纸层(img_wall_attach/show) */
#include "boot_builtin.h" /* 出厂内置开机图(未上传时默认显示) */
#include "provision.h"  /* 5×EN复位配网模式 */
#include "bootcnt.h"    /* 上电第一时间开机计数 (bfl_main 极早期已完成) */
#include "version.h"    /* 产品名/版本号/编译时间 (上电横幅) */

#define W  320
#define H  220
#define OX 0
#define OY 30
#define PIN_SCL 3
#define PIN_SDA 12
#define PIN_CS  14
#define PIN_DC  4
#define PIN_RST 17

static hosal_gpio_dev_t cs,dc,rst;

/* 硬件 SPI0 轮询模式: SCL=GPIO3(SCLK) SDA=GPIO12(MOSI), CS/DC/RST 仍由 GPIO 软件控制。
 * 注意: 不能走 hosal_spi_send —— 该路径中断驱动, 本芯片 ROM 构建下 SPI IRQ 不触发,
 * 会永久死锁; 这里用 bl602_std 寄存器级轮询 API (与已验证的 Arduino SPI 库同方案)。 */
static void spi_init(void)
{
    GLB_GPIO_Type pins[3];
    SPI_CFG_Type spicfg;
    SPI_FifoCfg_Type fifocfg;

    pins[0] = GLB_GPIO_PIN_3;   /* SCK  */
    pins[1] = GLB_GPIO_PIN_12;  /* MOSI */
    pins[2] = GLB_GPIO_PIN_17;  /* MISO (不用; RST 只在开机复位一次) */
    GLB_GPIO_Func_Init(GPIO_FUN_SPI, pins, 3);
    GLB_Set_SPI_0_ACT_MOD_Sel(GLB_SPI_PAD_ACT_AS_MASTER);

    GLB_AHB_Slave1_Reset(BL_AHB_SLAVE1_SPI);
    /* 8MHz → 40MHz: 全屏 121600B 刷新从 ~121ms 降到 ~24ms(5 倍),
     * 动画/时钟翻页都更丝滑; ST7789 标称 62MHz+, 40MHz 余量充足 */
    SPI_SetClock(SPI_ID_0, 40000000);
    SPI_SetDeglitchCount(SPI_ID_0, 0x2);

    memset(&spicfg, 0, sizeof(spicfg));
    spicfg.deglitchEnable   = DISABLE;
    spicfg.continuousEnable = ENABLE;
    spicfg.byteSequence     = SPI_BYTE_INVERSE_BYTE0_FIRST;
    spicfg.bitSequence      = SPI_BIT_INVERSE_MSB_FIRST;
    spicfg.frameSize        = SPI_FRAME_SIZE_32;
    spicfg.clkPhaseInv      = SPI_CLK_PHASE_INVERSE_0;   /* MODE0: 与原 bit-bang 相位一致 */
    spicfg.clkPolarity      = SPI_CLK_POLARITY_LOW;
    SPI_Init(SPI_ID_0, &spicfg);
    SPI_Disable(SPI_ID_0, SPI_WORK_MODE_MASTER);
    SPI_IntMask(SPI_ID_0, SPI_INT_ALL, MASK);   /* 轮询模式: 屏蔽中断 */

    fifocfg.txFifoThreshold = 1;
    fifocfg.rxFifoThreshold = 1;
    fifocfg.txFifoDmaEnable = DISABLE;
    fifocfg.rxFifoDmaEnable = DISABLE;
    SPI_FifoConfig(SPI_ID_0, &fifocfg);

    SPI_Enable(SPI_ID_0, SPI_WORK_MODE_MASTER);
}
static void _cmd(uint8_t c){ hosal_gpio_output_set(&dc,0);hosal_gpio_output_set(&cs,0);SPI_Send_8bits(SPI_ID_0,&c,1,SPI_TIMEOUT_ENABLE);hosal_gpio_output_set(&cs,1); }
static void _dat(uint8_t d){ hosal_gpio_output_set(&dc,1);hosal_gpio_output_set(&cs,0);SPI_Send_8bits(SPI_ID_0,&d,1,SPI_TIMEOUT_ENABLE);hosal_gpio_output_set(&cs,1); }
static void _win(uint16_t x1,uint16_t y1,uint16_t x2,uint16_t y2){
    x1+=OX;x2+=OX;
    _cmd(0x2A);_dat(x1>>8);_dat(x1);_dat(x2>>8);_dat(x2);
    _cmd(0x2B);_dat((y1+OY)>>8);_dat(y1+OY);_dat((y2+OY)>>8);_dat(y2+OY);
    _cmd(0x2C);
}
static void my_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color){
    _win(area->x1,area->y1,area->x2,area->y2);
    hosal_gpio_output_set(&dc,1);hosal_gpio_output_set(&cs,0);
    uint32_t w=(uint32_t)(area->x2-area->x1+1);
    uint32_t h=(uint32_t)(area->y2-area->y1+1);
    uint8_t *p=(uint8_t*)color;
    /* LVGL buffer 小端(SWAP=0), ST7789 要高字节在前: 逐行交换后整行 SPI 发送 */
    static uint8_t line[W*2];
    for(uint32_t y=0;y<h;y++){
        for(uint32_t x=0;x<w;x++){ line[x*2]=p[1]; line[x*2+1]=p[0]; p+=2; }
        SPI_Send_8bits(SPI_ID_0,line,w*2,SPI_TIMEOUT_ENABLE);
    }
    hosal_gpio_output_set(&cs,1);
    lv_disp_flush_ready(drv);
}
static void tick_cb(void *arg){ lv_tick_inc(1); }

/* ---- WiFi 并行启动(开机图期间) ----
 * 必须在 boot_logo_show 的 SPI 刷屏完成后才调用: hal_wifi_start_firmware_task
 * 的射频校准是高优先级长任务, 若在刷屏前启动会抢占 SPI, 导致 logo
 * 只渲染一半就黑屏("logo 闪一下")。渲染完成后再启, logo 完整上屏 2 秒,
 * 校准/连接全部在 2 秒内并行完成。
 * 注意: cfg_load + tcpip_init 由 main() 无条件完成(配网模式 AP 也依赖
 * lwIP), 本函数只负责正常模式下的 STA 连接任务。 */
static void wifi_entry(void *pv);
static void conn_timeout_task(void *arg);
static void media_erase_task(void *arg);   /* 定义在 factory_reset_if_new 后 */

/* ---- boot 启动屏共用状态(定义在 boot_logo_show 之前, 供其引用) ---- */
int  g_ntp_done = 0;            /* NTP 首次对时完成: boot 屏等待联网结束, 直接跳真实时间 */
static int g_wifi_fail = 0;     /* 收到过 WiFi 断开事件(底部状态小字显示"连接失败") */
static volatile int g_got_ip=0; /* 上电后是否连上过 WiFi (连接超时判定用) */
static volatile int g_force_full=0; /* 兜底超时(8s/45s): 强制进度条爬满 100% 才退出 */

/* 裸刷进度条(纯 SPI 绘制, 不依赖 LVGL): 位置 (16,150) 288x4,
 * 轨道深色 0x1E2338 + 指示器时钟蓝 0x2196f3(主界面时钟同色);
 * 颜色为 RGB565 大端字节对(与 my_flush 交换后一致, 直接送 SPI) */
#define BAR_X 16
#define BAR_Y 150
#define BAR_W 288
#define BAR_H 4
static const uint8_t bar_col_bg[2] = { 0x19, 0x07 }; /* 0x1E2338 → RGB565 大端 */
static const uint8_t bar_col_fg[2] = { 0x24, 0xBE }; /* 0x2196f3 → RGB565 大端 */
static int g_bar_on = 0;        /* 本次启动是否显示进度条(仅正常联网模式) */
static int s_bar_px = -1;       /* 已刷到屏幕的填充列数(-1 = 轨道尚未绘制) */
static volatile int s_media_erase_pending = 0; /* 出厂重置待后台擦 media(logo 上屏后并行) */

/* ---- 全局状态(boot 启动屏与主循环共用, 定义在 boot_logo_show 之前) ---- */
gg_ui_t guider_ui;
SemaphoreHandle_t lvgl_mutex;   /* weather.c 共享 */
int g_wall_vis = 0;             /* 图播放: 当前是否只显示壁纸(xcmd 上传完成时置 1) */
int g_wifi_fw_started = 0;      /* WiFi 固件任务/PM 是否已启动 (只能初始化一次, provision.c 共用) */
char g_time_str[32] = "Connecting...";
int  g_wday = -1;               /* 星期 0=周日..6=周六, SNTP 同步后有效 */
static void wifi_start_parallel(void)
{
    if (g_cfg.ssid[0] != 0) {
        xTaskCreate(wifi_entry,"wifi",1024*3,NULL,15,NULL);
        prov_keepalive_start();
        xTaskCreate(conn_timeout_task, "conn_to", 1024 * 2, NULL, 5, NULL);
    }
}

/* ---- 开机启动屏: 全屏开机图 + 连接进度条 ----
 * 纯 SPI 裸刷版(不依赖 LVGL, lcd_init 复位后秒出): 开机图逐行直刷
 * (RGB565 小端→大端交换后整行发送, 与旧版 v0.1.4 一致), 进度条也由
 * SPI 直接绘制 —— 每 20ms 只刷增长的那几列, 视觉与 LVGL 版一致。
 * 等待联网完成(GOT_IP + NTP 对时)后退出 → main 直接切时钟屏, 首帧即
 * 真实时间, 不再出现 88:88 占位时钟界面。
 * 退出条件:
 *   - 配网/出厂: logo 固定 2 秒后退出(进配网页)
 *   - 正常模式: NTP 对时成功 → 立即退出(logo 直接跳时间);
 *     GOT_IP 后 8s 仍未对时 → 兜底退出;conn_timeout_task(45s) 触发
 *     prov_enter → prov_is_active → 退出(进配网页)
 * 等待期间最长 45s, 必须喂看门狗。 */
static void boot_bar_track(void)
{
    static uint8_t line[BAR_W * 2];
    for (int i = 0; i < BAR_W; i++) { line[i*2] = bar_col_bg[0]; line[i*2+1] = bar_col_bg[1]; }
    _win(BAR_X, BAR_Y, BAR_X + BAR_W - 1, BAR_Y + BAR_H - 1);
    hosal_gpio_output_set(&dc, 1);
    hosal_gpio_output_set(&cs, 0);
    for (int y = 0; y < BAR_H; y++)
        SPI_Send_8bits(SPI_ID_0, line, BAR_W * 2, SPI_TIMEOUT_ENABLE);
    hosal_gpio_output_set(&cs, 1);
}
static void boot_bar_fill(int from, int to)
{
    static uint8_t line[BAR_W * 2];
    for (int i = 0; i < to - from; i++) { line[i*2] = bar_col_fg[0]; line[i*2+1] = bar_col_fg[1]; }
    _win(BAR_X + from, BAR_Y, BAR_X + to - 1, BAR_Y + BAR_H - 1);
    hosal_gpio_output_set(&dc, 1);
    hosal_gpio_output_set(&cs, 0);
    for (int y = 0; y < BAR_H; y++)
        SPI_Send_8bits(SPI_ID_0, line, (to - from) * 2, SPI_TIMEOUT_ENABLE);
    hosal_gpio_output_set(&cs, 1);
}
static void boot_st_update(void)
{
    /* 进度条动画: 从 1% 平滑递增到 100%, 前段快、末段慢 ——
     * 连接 WiFi 中: 中速爬向 60% (0.4%/20ms ≈ 20%/s);
     * 已连等 NTP: 快速爬向 92% (1.2%/20ms ≈ 60%/s);
     * 对时完成/兜底超时: 快补到 92%, 最后 92→100 慢速磨满 (0.2%/20ms ≈ 10%/s),
     * 满格后 boot_logo_wait 才允许退出 —— 任何路径都必须 100% 才跳转。
     * val 用 0.1% 精度避免 20ms 一档的舍入感。 */
    static int val = 10;            /* 1% 起步 */
    if (!g_bar_on) return;
    int cap, step;
    if (g_ntp_done || prov_is_active() || g_force_full) {
        /* 对时完成 / 运行时进配网 / 兜底超时: 强制补满 */
        if (val < 920) { cap = 920; step = 40; }
        else           { cap = 1000; step = 2; }
    } else if (g_got_ip) {
        /* 已连等 NTP: 快速爬向 92% */
        cap = 920; step = 12;
    } else {
        /* 连接中: 中速爬向 60% (前段快) */
        cap = 600; step = 4;
    }
    if (val < cap) {
        val += step;
        if (val > cap) val = cap;
    }
    /* 裸刷增量: 只刷新增的填充列(轨道已由 boot_bar_track 画好) */
    int px = BAR_W * ((val + 5) / 10) / 100;
    if (px > s_bar_px) {
        boot_bar_fill(s_bar_px, px);
        s_bar_px = px;
    }
}
static void boot_logo_wait(void)
{
    uint32_t t0 = xTaskGetTickCount(), got_ip_t = 0;
    while (1) {
        bl_wdt_feed();              /* 等待最长 45s: 期间必须喂看门狗 */
        boot_st_update();           /* 裸刷进度条(纯 SPI, 无 LVGL) */
        uint32_t t = xTaskGetTickCount();
        int bar_full = (g_bar_on && s_bar_px >= BAR_W);
        if (prov_is_active()) {
            /* 上电配网模式(无进度条): logo 停 2 秒;
             * 运行时进配网(45s 超时): 进度条爬满后才退出进配网页 */
            if (!g_bar_on) { if (t - t0 >= 2000) break; }
            else if (bar_full) break;
        } else if (g_cfg.ssid[0] == 0) {            /* 出厂无配置: 2 秒后进配网 */
            if (t - t0 >= 2000) break;
        } else {                                    /* 正常模式: 等联网完成 */
            if (g_ntp_done && bar_full) break;      /* 对时完成 + 进度条满格: 跳时间 */
            if (!got_ip_t && g_got_ip) got_ip_t = t;
            /* 已连但 NTP 8s 未对时: 强制爬满 100% 后再退出 */
            if (got_ip_t && t - got_ip_t > 8000) g_force_full = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    /* 出厂重置: 等后台 media 擦除收尾(正常模式在 WiFi 等待期已并行完成;
     * 配网/出厂 2s 路径最多多等 ~1-2s) —— 时钟屏读壁纸需要完整分区 */
    while (s_media_erase_pending) {
        bl_wdt_feed();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static void boot_logo_show(void)
{
    const uint8_t *p = NULL;
    uint32_t sz = 0;
    const char *v = ef_get_env("w_bootimg");
    const char *on = ef_get_env("w_booton");

    if (on != NULL && on[0] == '0') {   /* 开关关闭: 不显示 logo, 但正常模式 WiFi 照常并行 */
        if (!prov_is_active())
            wifi_start_parallel();
        return;
    }
    if (v != NULL && v[0] == '1' &&
        img_boot_get((const void **)&p, &sz) == 0 && p && sz >= IMG_BOOT_BYTES) {
        /* 已上传开机图: media 分区 XIP */
    } else {
        /* 出厂默认启动页: 内置安信可 logo */
        p = boot_builtin;
        sz = BOOT_BUILTIN_BYTES;
    }
    (void)sz;

    /* 纯 SPI 裸刷全屏开机图(逐行 RGB565 小端→大端交换后整行发送):
     * 不走 LVGL 渲染, lcd_init 复位后秒出(与旧版 v0.1.4 裸刷一致) */
    static uint8_t line[W * 2];
    _win(0, 0, W - 1, H - 1);
    hosal_gpio_output_set(&dc, 1);
    hosal_gpio_output_set(&cs, 0);
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) { line[x*2] = p[1]; line[x*2+1] = p[0]; p += 2; }
        SPI_Send_8bits(SPI_ID_0, line, W * 2, SPI_TIMEOUT_ENABLE);
    }
    hosal_gpio_output_set(&cs, 1);
    printf("[TIME] %lu logo on screen\r\n", (unsigned long)xTaskGetTickCount());

    /* 出厂重置(新固件首启动): 后台擦 media —— 不与 logo 抢黑屏时间 */
    if (s_media_erase_pending)
        xTaskCreate(media_erase_task, "m_erase", 1024 * 2, NULL, 8, NULL);

    /* 连接进度条(裸 SPI 绘制, 无文字): 仅正常联网模式显示 —— 配网模式
     * (5 次 EN)和出厂无配置(烧录后首次开机, 2 秒后进配网页)的 logo 都
     * 保持干净(纯 logo); 位置原状态小字处 (16,150), 高 4 细条,
     * 可视区 0~189 内; 轨道深色底 + 指示器填充时钟蓝 0x2196f3
     * (主界面时钟同色) */
    g_bar_on = 0;
    if (!prov_is_active() && g_cfg.ssid[0] != 0) {
        g_bar_on = 1;
        boot_bar_track();       /* 画轨道(深色底), 填充由 boot_st_update 增量补 */
        s_bar_px = 0;
    }

    /* 正常模式: 启 STA 并行连接(校准/连接与 logo 显示并行);
     * 配网模式: 不启 STA, AP 由 prov_start 负责(等待循环退出后) */
    if (!prov_is_active())
        wifi_start_parallel();

    boot_logo_wait();           /* 等联网完成/2 秒: 期间进度条实时反映连接阶段 */
}

/* 时钟 hour/minute 文本同步(仅当真正变化时; SNTP 未同步时 g_time_str 是
 * "Connecting...", 保持 88:88 占位不覆盖)。时钟首帧渲染前调用一次:
 * 联网已完成(对时通过)时, logo 直接跳到真实时间, 不闪现 88:88 */
static void clock_text_sync(void)
{
    static char prev_h[3], prev_m[3];
    if(g_time_str[4]=='-' && !img_wall_anim_busy()){
        char h[3]={g_time_str[11],g_time_str[12],0};
        char m[3]={g_time_str[14],g_time_str[15],0};
        if(strcmp(h,prev_h) || strcmp(m,prev_m)){
            strcpy(prev_h,h); strcpy(prev_m,m);
            lv_textarea_set_text(guider_ui.screen.hour,h);
            lv_textarea_set_text(guider_ui.screen.minute,m);
        }
    }
}
/* 出厂重置标记 (version.h FW_RESET_MARK): 每次重新编译烧录, 首次开机
 * 自动清空旧设置+媒体分区恢复出厂 (进配网), 之后重刷同版本不再清。 */


static void lcd_init(void)
{
    cs.config =OUTPUT_PUSH_PULL;cs.port =PIN_CS; hosal_gpio_init(&cs);
    dc.config =OUTPUT_PUSH_PULL;dc.port =PIN_DC; hosal_gpio_init(&dc);
    rst.config=OUTPUT_PUSH_PULL;rst.port=PIN_RST;hosal_gpio_init(&rst);
    hosal_gpio_output_set(&cs,1);hosal_gpio_output_set(&dc,1);
    hosal_gpio_output_set(&rst,0);vTaskDelay(50);
    hosal_gpio_output_set(&rst,1);vTaskDelay(150);
    spi_init();                 /* RST 一次性复位已完成, GPIO3/12/17 复用为 SPI0 */
    _cmd(0x01);vTaskDelay(150);_cmd(0x11);vTaskDelay(120);
    _cmd(0x36);_dat(0x60);_cmd(0x3A);_dat(0x55);
    {uint8_t d[]={0x0C,0x0C,0x00,0x33,0x33};_cmd(0xB2);for(int i=0;i<5;i++)_dat(d[i]);}
    _cmd(0xB7);_dat(0x35);_cmd(0xBB);_dat(0x19);_cmd(0xC0);_dat(0x2C);
    _cmd(0xC2);_dat(0x01);_cmd(0xC3);_dat(0x12);_cmd(0xC4);_dat(0x20);
    _cmd(0xC6);_dat(0x0F);_cmd(0xD0);_dat(0xA4);_dat(0xA1);
    {uint8_t d[]={0xD0,0x00,0x02,0x07,0x0A,0x28,0x32,0x44,0x42,0x06,0x0E,0x12,0x14,0x17};_cmd(0xE0);for(int i=0;i<14;i++)_dat(d[i]);}
    {uint8_t d[]={0xD0,0x00,0x02,0x07,0x0A,0x28,0x31,0x54,0x47,0x0E,0x1C,0x17,0x1B,0x1E};_cmd(0xE1);for(int i=0;i<14;i++)_dat(d[i]);}
    _cmd(0x21);_cmd(0x13);_cmd(0x29);vTaskDelay(50);
    _cmd(0x2A);_dat(0);_dat(0);_dat(0x01);_dat(0x3F);
    _cmd(0x2B);_dat(0);_dat(0);_dat(0x01);_dat(0x3F);
    _cmd(0x2C);hosal_gpio_output_set(&dc,1);hosal_gpio_output_set(&cs,0);
    static uint8_t zeros[1024];
    for(uint32_t i=0;i<320*320*2;i+=sizeof(zeros))
        SPI_Send_8bits(SPI_ID_0,zeros,sizeof(zeros),SPI_TIMEOUT_ENABLE);
    hosal_gpio_output_set(&cs,1);
}


/* ===== 天气轮询(双源: 高德/和风) 见 custom/weather.c ===== */

/* 出厂重置: FW_VER 变化时首次启动执行。
 * 1) 清空 easyflash 全部设置键(WiFi/城市/壁纸/背景/开机图开关/开机计数)
 * 2) 擦除 media 分区(用户上传的开机图/壁纸) → 恢复出厂内置 logo/壁纸
 * 之后一切用编译期默认值, 相当于全新出厂固件。
 * 注意: 清键用 ef_env_set_default() 一次整区擦除重写(逐键 ef_del_env
 * 22 次实测 3.9s, 且把 env 扇区写乱导致后续 cfg_load 读取再慢 3s);
 * media 擦除 290KB 是另一大头, 置 s_media_erase_pending 后由
 * media_erase_task 在开机图裸刷显示后并行执行(flash 擦除不经 SPI 总线) */
static void factory_reset_if_new(void)
{
    const char *ver = ef_get_env("w_fwver");
    if (ver != NULL && strcmp(ver, FW_RESET_MARK) == 0) return;

    /* 整体擦除 env 分区并重建默认表: 清掉全部设置键(含配网保持标志
     * w_plast —— 出厂重置必须退出配网模式, 否则每次开机直接 resume
     * 配网, 永远看不到开机图); 开机计数存 media 扇区不在此列, 由
     * media_erase_task 整区擦除连带清除(先读回再恢复) */
    ef_env_set_default();
    ef_set_and_save_env("w_fwver", FW_RESET_MARK);
    s_media_erase_pending = 1;
    printf("[CFG] factory reset pending (fw=%s)\r\n", FW_RESET_MARK);
}

/* 后台擦 media(出厂重置): logo 裸刷显示后启动, 与 WiFi 连接/进度条动画
 * 并行; 擦除完成恢复 <5 的开机计数(boot_logo_wait 退出前等待其收尾,
 * 保证时钟屏读壁纸时 media 分区是完整的) */
static void media_erase_task(void *arg)
{
    (void)arg;
    int boot_cnt = bootcnt_get();           /* 擦媒体前读回开机计数 */
    bl_mtd_handle_t h = NULL;
    if (bl_mtd_open("media", &h, BL_MTD_OPEN_FLAG_NONE) == 0) {
        bl_mtd_erase(h, 0, 0x47000);        /* media 分区整区 290KB */
        bl_mtd_close(h);
    }
    /* 未达配网阈值(<5)的计数恢复, 不打断连按复位进配网的测试方式;
     * 已 >=5 的计数是崩溃/反复重启累积的残留(触发配网的使命早已完成,
     * 本应被消费), 恢复它只会让出厂重置后仍被强制进配网 —— 直接清零 */
    if (boot_cnt > 0 && boot_cnt < PROV_NEED_COUNT)
        bootcnt_restore((uint32_t)boot_cnt);
    s_media_erase_pending = 0;
    printf("[CFG] media erased (factory reset done)\r\n");
    vTaskDelete(NULL);
}

/* ===== Claude Code 状态灯 =====
 * 状态由串口 #XLAMP 命令推送(EXE 端读取状态文件后下发):
 *   0=黄色呼吸 1/2=黄色常亮 3=绿色常亮 4=红色常亮
 * #XMON 控制开关, 关闭时恢复默认蓝
 */

/* ---- 时间显示: 新 UI(TEST3)hour/minute/point textarea 直接显示, 无翻页动画 ---- */

/* ===== SNTP 获取时间 + 离线走时校准 =====
 * sntp_get_time 返回 ntp_sec + tick 推进(同一时钟源), ntp_sec 只在真实
 * NTP 同步时跳变。用 frag(ms)精度对比本地 tick 外推值, 跳变 >1.5s 即
 * 判定为一次真实对时, 跳变量 = 该对时间隔内本地累计漂移 → 估算漂移
 * 速率(us/s), 断网离线走时按此补偿, 保证脱网数天内时间仍基本准确。 */
static void sntp_start(void *arg)
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    /* 0: 阿里云NTP直连IP, lwIP dns_gethostbyname 对点分IP直接返回, 免DNS查询 */
    sntp_setservername(0, "203.107.6.88");
    /* 1: 域名兜底, IP 变更或 0 号超时时自动切换 (SNTP_MAX_SERVERS=2) */
    sntp_setservername(1, "ntp.aliyun.com");
    sntp_init();
}

static int32_t  s_drift_us_per_s = 0;   /* 本地每 NTP 秒偏快(+)的微秒数 */
static uint64_t s_anchor_ms = 0;        /* 最近一次有效对时后的 NTP 毫秒 */
static uint32_t s_anchor_tick = 0;      /* 对应本地 tick(ms) */

static void sntp_task(void *pv)
{
    static int time_shown = 0;      /* 首次显示时间 → 清零开机计数 */
    tcpip_callback(sntp_start, NULL);
    vTaskDelay(pdMS_TO_TICKS(500));     /* SNTP_STARTUP_DELAY=0 已去掉随机延时, 500ms 足够发首包 */
    while(1){
        uint64_t now_ms = 0;
        /* 用 SDK 系统毫秒时间 (sntp 同步时由 sntp_process 写入精确服务器时间,
         * FreeRTOS tick 连续推进, 见 bl_sys_time.c) —— 绕开 sntp_get_time 的
         * 整秒取整 + 坏 frag, 那两个问题造成 ±1 秒随机相位抖动 (显示慢 0.5~1s) */
        if (bl_sys_time_get(&now_ms) == 0 && now_ms > 1000000000000ull) {
            /* 锚点推进与系统时间同源 (FreeRTOS tick): jump 只剩真实 NTP 漂移 */
            uint32_t tick  = xTaskGetTickCount();
            uint32_t el    = (uint32_t)((int32_t)tick - (int32_t)s_anchor_tick); /* wrap 安全 */

            if (s_anchor_tick == 0) {
                s_anchor_ms = now_ms;                /* 首次对时: 建立锚点 */
                s_anchor_tick = tick;
            } else {
                int64_t extrap = (int64_t)s_anchor_ms + el;
                int64_t jump   = (int64_t)now_ms - extrap;   /* 对时校正量 */
                /* 每次轮询都把锚点无条件拉回 NTP: 本地 tick (BL602 内部
                 * RC 晶振漂移实测 ~0.6%/分钟级) 与 NTP 的偏差无论大小都
                 * 必须修正, 否则显示时间持续偏快/偏慢不被纠正 */
                s_anchor_ms = now_ms;
                s_anchor_tick = tick;
                if (el > 5000 && (jump > 300 || jump < -300)) {
                    /* 学习本地漂移 (阈值 300ms: 覆盖普通晶振漂移, 过滤网络抖动):
                     * 平滑更新 7 成新值 + 3 成旧值, 防单次抖动毛刺带偏 */
                    int32_t d = (int32_t)(jump * 1000 / el);
                    if (d > -1000 && d < 1000) {
                        s_drift_us_per_s = s_drift_us_per_s * 3 / 10 + d * 7 / 10;
                        printf("[SNTP] drift=%dus/s (jump=%lldms/%ums)\r\n",
                               s_drift_us_per_s, (long long)jump,
                               (unsigned long)el);
                    } else {
                        printf("[SNTP] drift %dus/s out of range, ignored\r\n", d);
                    }
                } else if (jump > 200 || jump < -200) {
                    printf("[SNTP] micro jump=%lldms\r\n", (long long)jump);
                }
            }
            /* 校正后时间(离线时按漂移补偿推进) */
            el = (uint32_t)((int32_t)tick - (int32_t)s_anchor_tick);
            /* 注: 显示字符串按 dsec 变化才更新, 轮询周期必须短于 1 秒,
             * 否则分钟切换滞后 0~1 秒 (平均 0.5s) —— 100ms 轮询滞后 <0.1s */
            int64_t disp_ms = (int64_t)s_anchor_ms + el +
                              (int64_t)el * s_drift_us_per_s / 1000000;
            {
                static uint32_t last_disp_sec = 0;
                uint32_t dsec = (uint32_t)(disp_ms / 1000);
                if (dsec != last_disp_sec) {
                    last_disp_sec = dsec;
                    utils_time_date_t d;
                    utils_time_date_from_epoch(dsec + 8*3600, &d);
                    /* 星期: 1970-01-01 是周四, 0=周日 */
                    g_wday = (int)(((dsec + 8*3600) / 86400 + 4) % 7);
                    snprintf(g_time_str,sizeof(g_time_str),
                        "%04d-%02d-%02d %02d:%02d:%02d",
                        d.ntp_year,d.ntp_month,d.ntp_date,
                        d.ntp_hour,d.ntp_minute,d.ntp_second);
                    if (!time_shown) {  /* 时钟正常显示 = 正常使用: 清零计数 */
                        time_shown = 1;
                        g_ntp_done = 1;     /* 联网完成: boot 屏等待结束, 直接跳时间 */
                        bootcnt_clear();
                        printf("[PROV] count reset (time shown)\r\n");
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));     /* 100ms: 显示秒级刷新, 分钟切换滞后 <0.1s */
    }
}

/* ===== WiFi ===== */
static wifi_conf_t wconf={.country_code="CN"};
static uint8_t svc_started=0;   /* GOT_IP 服务守卫,防重连后重复建任务 */
#define CONN_TO_MS (45 * 1000)  /* 上电连接超时: SDK 连接失败后每 ~2s 自动重连(全信道
                                 * 扫描一轮约 3~6s), 45s ≈ 8~10 轮重试, 仍连不上
                                 * (路由器故障/密码被改/信号到不了) → 自动进配网 */
static void wifi_connect(char *s,char *p){
    wifi_interface_t iface=wifi_mgmr_sta_enable();
    wifi_mgmr_sta_connect(iface,s,p,NULL,NULL,0,0);
}

/* 仅上电这一次等待: 已配置 WiFi 45s 内无 GOT_IP → 自动进配网重配。
 * 连上之后的运行中断网不算, 走离线时钟 */
static void conn_timeout_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CONN_TO_MS));
    if (!g_got_ip && !prov_is_active())
        prov_enter();   /* provision.c: 停 STA + 切配网 UI + 启 AP */
    vTaskDelete(NULL);
}

static void wifi_cb(input_event_t *ev,void *pv){
    switch(ev->code){
        case CODE_WIFI_ON_INIT_DONE:
            wifi_mgmr_start_background(&wconf); break;
        case CODE_WIFI_ON_MGMR_DONE:
            wifi_connect(g_cfg.ssid,g_cfg.pwd); break;
        case CODE_WIFI_ON_GOT_IP:
            printf("[WiFi] GOT IP\r\n");
            /* DHCP 已用路由器 DNS 填了 0/1 号位, 在此把阿里公共DNS 设到 0 号位
             * (1 号位保留路由器 DNS 兜底), 加速天气等域名的解析 */
            {
                ip4_addr_t dns;
                IP4_ADDR(&dns, 223, 5, 5, 5);
                dns_setserver(0, &dns);
            }
            g_got_ip=1;
            g_wifi_fail=0;      /* 连接成功: 清除失败标志(boot 状态小字) */
            xcmd_send_event("GOT_IP",xcmd_ip_str());
            if(!svc_started){
                svc_started=1;
                xTaskCreate(sntp_task,"sntp",1024*2,NULL,15,NULL);
                if (g_cfg.wx_on) {   /* 天气默认关闭(wx_on=0), 代码保留 */
                    if (xTaskCreate(weather_task,"wx",1024*3,NULL,5,NULL) != pdPASS)
                        printf("[WX] task create fail\r\n"); /* mbedtls 上下文在堆上, 3KB 栈够 */
                }
            }
            break;
        case CODE_WIFI_ON_DISCONNECT: {
            g_wifi_fail = 1;    /* boot 状态小字: "连接失败, 重连中"(GOT_IP 时清除) */
            /* 打印断线原因(便于排查偶发连接失败): 状态码由 SDK 置位,
             * 常见 3=4way 握手超时/密码错, 7=未找到 AP, 10=认证失败 */
            int sc = 0;
            wifi_mgmr_status_code_get(&sc);
            printf("[WiFi] DISCONNECT (%s)\r\n", wifi_mgmr_status_code_str((uint16_t)sc));
            xcmd_send_event("WIFI_DISCONNECT",g_cfg.ssid);
            break;
        }
        default: break;
    }
}
/* 时钟屏背景应用(开机 cfg_load 后调用; #XBG 串口命令也会调用):
 * guider screen 创建时未设背景(默认白), 且换屏后 lv_scr_act 的黑底已失效,
 * 背景色必须直接设置在 screen 自身。样式: 0=纯色(c1) 1=水平渐变 2=垂直渐变 */
void bg_apply(void)
{
    /* 时钟页背景: 默认淡蓝→淡紫垂直渐变; 渐变方向为 bg_grad_color(起) → bg_color(止) */
    lv_obj_set_style_bg_color(guider_ui.screen.screen, lv_color_hex(g_cfg.bg_c2), 0);
    lv_obj_set_style_bg_grad_color(guider_ui.screen.screen, lv_color_hex(g_cfg.bg_c1), 0);
    lv_obj_set_style_bg_opa(guider_ui.screen.screen, LV_OPA_COVER, 0);
    switch (g_cfg.bg_dir) {
        case 0: /* 纯色: 只用起色 */
            lv_obj_set_style_bg_color(guider_ui.screen.screen, lv_color_hex(g_cfg.bg_c1), 0);
            lv_obj_set_style_bg_grad_dir(guider_ui.screen.screen, LV_GRAD_DIR_NONE, 0);
            break;
        case 1:
            lv_obj_set_style_bg_grad_dir(guider_ui.screen.screen, LV_GRAD_DIR_HOR, 0);
            break;
        default: /* 2: 垂直渐变(默认) */
            lv_obj_set_style_bg_grad_dir(guider_ui.screen.screen, LV_GRAD_DIR_VER, 0);
            break;
    }
}

static void wifi_entry(void *pv){
    aos_register_event_filter(EV_WIFI,wifi_cb,NULL);
    hal_wifi_start_firmware_task();
    g_wifi_fw_started = 1;      /* PM/固件任务只允许启动一次, 运行时进配网不能重复启动 */
    aos_post_event(EV_WIFI,CODE_WIFI_ON_INIT_DONE,0);
    vTaskDelete(NULL);
}

/* ===== 上电横幅: 产品名 + 版本 + 编译时间 + git + MAC + 固件大小 + 作者 =====
 * MAC 从 efuse/flash 读取(与配网页同源), 未烧录时回退默认值
 * 固件大小: 链接脚本 PROVIDE 符号 _fw_size (flash 加载区总长, 实测=bin 大小) */
extern uint32_t _fw_size;
#define FW_PART_SIZE 0x160000u /* FW 分区上限, 见 partition_cfg_2M.toml (1408 KiB) */

static void boot_banner_show(void)
{
    uint8_t mac[6];
    char mac_str[18] = "--:--:--:--:--:--";
    unsigned long fw_size;

    if (wifi_mgmr_sta_mac_get(mac) == 0)
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    fw_size = (unsigned long)&_fw_size;

    /* 横幅: 上下各一行分隔线, 中间为产品/版本信息 (无图标) */
    printf("========================================\r\n");
    printf("  %s\r\n", PRODUCT_NAME);
    printf("  FW %s  (build %s %s)\r\n", FW_VER_STR, FW_BUILD_DATE, FW_BUILD_TIME);
    printf("  git: %s (%s%s)\r\n", FW_GIT_HASH, FW_GIT_BRANCH,
           FW_GIT_DIRTY ? " +dirty" : "");
    printf("  MAC: %s\r\n", mac_str);
    printf("  CPU: %uMHz  heap %lu B\r\n",
           (unsigned)(BL_RD_WORD(0x4000F108) / 1000000),
           (unsigned long)xPortGetFreeHeapSize());
    printf("  fw: %lu KiB / 1408 KiB\r\n", fw_size >> 10);
    printf("  boot: %u / 5\r\n", (unsigned)bootcnt_get());
    printf("  ef: EasyFlash %s\r\n", EF_SW_VERSION);
    printf("  by xemowo (xemowo@qq.com)\r\n");
    printf("========================================\r\n");
}

void main(void)
{
    lvgl_mutex = xSemaphoreCreateMutex();
    bl_sys_init();

    boot_banner_show();
    printf("[TIME] %lu banner done\r\n", (unsigned long)xTaskGetTickCount());

    /* 开机计数已在 bfl_main 极早期完成 (bootcnt.c app_boot_early, "Booting"
     * 横幅之前, 连续按 EN 每下都立刻计入, 不被任何初始化挡住);
     * 这里只做出厂检查 + 判断计数是否达到 5 次 */
    factory_reset_if_new(); /* 新固件首次启动: 清旧设置 (media 擦除后移 logo 后) */
    prov_boot_check();      /* 累计 5 次 → 配网模式 */
    bl_wdt_init(30000);     /* 硬件看门狗 30s: 死机自动复位(主循环喂狗) */
    printf("[TIME] %lu factory/bootcnt done\r\n", (unsigned long)xTaskGetTickCount());

    lcd_init();
    printf("[TIME] %lu lcd init done\r\n", (unsigned long)xTaskGetTickCount());
    /* 配置与 lwIP 无条件初始化: 配网模式 AP 也依赖 tcpip(prov_start 直接
     * 启固件任务+AP, 不再走本处)。正常模式的 STA 并行连接由 boot_logo_show
     * 在 logo 裸刷完成后启动(见 wifi_start_parallel): 先完整刷完 logo
     * (纯 SPI, 无抢占), 再启 WiFi —— logo 显示与校准/连接并行, 无黑窗 */
    cfg_load(&g_cfg);
    tcpip_init(NULL,NULL);

    /* LVGL 提前初始化(几十 ms): boot 屏本身纯 SPI 裸刷不依赖 LVGL, 但
     * 45s 运行时进配网的 prov_ui_init 需要 LVGL 已就绪 */
    lv_init();
    printf("[TIME] %lu lvgl init done\r\n", (unsigned long)xTaskGetTickCount());
    static lv_disp_draw_buf_t dbuf;
    static lv_color_t buf1[W*20];
    lv_disp_draw_buf_init(&dbuf,buf1,NULL,W*20);
    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res=W;drv.ver_res=H;drv.flush_cb=my_flush;drv.draw_buf=&dbuf;
    lv_disp_drv_register(&drv);

    static hosal_timer_dev_t tdev={
        .config={.cb=tick_cb,.period=1000,.reload_mode=TIMER_RELOAD_PERIODIC},.port=0};
    hosal_timer_init(&tdev);hosal_timer_start(&tdev);

    boot_logo_show();   /* 任何模式: logo + WiFi/NTP 状态 → 联网完成或 2 秒后退出
                         * (正常模式等待对时完成再走, 配网/出厂固定 2 秒) */
    printf("LCD ok, heap=%lu\r\n",(unsigned long)xPortGetFreeHeapSize());

    setup_ui(&guider_ui);
    custom_init(&guider_ui);

    /* 时钟 textarea 直接显示(Morganite 字体), 去掉可点击/光标,
     * 避免时钟数字上出现闪烁光标 */
    lv_obj_t *clock_tas[] = {
        guider_ui.screen.hour, guider_ui.screen.minute, guider_ui.screen.point,
        guider_ui.screen.moon, guider_ui.screen.day,    guider_ui.screen.slash,
        guider_ui.screen.week, guider_ui.screen.week_num,
    };
    for (unsigned i = 0; i < sizeof(clock_tas) / sizeof(clock_tas[0]); i++) {
        lv_obj_clear_flag(clock_tas[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(clock_tas[i], LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_DEFAULT);
        lv_obj_set_style_opa(clock_tas[i], LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_DEFAULT);
    }

    /* 首帧渲染移到下方分支之后: 配网路径直接渲染配网页(跳过时钟界面
     * 黑屏帧, logo 2 秒后直接进配网页), 正常路径渲染时钟首帧
     * (WiFi 已在 logo 2 秒内并行连接, 若 SNTP 已对时, 首帧就是真实时间) */
    printf("UI ok, heap=%lu\r\n",(unsigned long)xPortGetFreeHeapSize());
    printf("[MEM] Flash: ~791KB used / 2MB | RAM free heap: %lu B\r\n",
           (unsigned long)xPortGetFreeHeapSize());

    /* 串口命令协议(flash 配置已在上方 lcd_init 后读取;
     * 此前 lcd_init 的延时已让 yloop 完成 vfs/easyflash 初始化) */
    /* 时钟屏背景: 新 UI 设计自带黑底(gg_screen.c), 开机不再用渐变覆盖;
     * #XBG 串口命令仍可运行时修改(bg_apply 由 xcmd.c 调用) */
    xcmd_init();

    if (prov_is_active()) {
        prov_ui_init();             /* 配网模式: 切到配网界面 (必须在 prov_boot_check 之后) */
        lv_timer_handler();         /* 立即渲染配网页: logo 后直接进配网, 无黑屏帧 */
    } else if (g_cfg.ssid[0] == 0) {
        /* 出厂无 WiFi 配置: 不建壁纸层, 直接进配网(不等 20 秒);
         * 需在 lv_init/UI 之后(prov_ui_init 用 LVGL) */
        prov_enter();
        lv_timer_handler();         /* 立即渲染配网页: logo 2 秒后直进配网, 跳过时钟界面黑屏帧 */
    } else {
        /* 壁纸层(全屏最底层): 开机默认显示时钟, 壁纸由图播放轮播(#XWALL 开启)切换;
         * 总是绑定图源: 已上传过图用 media 分区 XIP 图, 否则用出厂内置壁纸
         * (custom/wall_builtin.c, 无需上位机烧图) */
        img_wall_attach(lv_scr_act());
        img_wall_bind();
        clock_text_sync();          /* 对时已完成: 首帧即真实时间, 不闪现 88:88 */
        lv_timer_handler();         /* 立即渲染时钟首帧: logo 直接切到时钟, 无黑屏窗口 */
    }

    /* 配网模式的网络服务(正常模式的 wifi_entry 已在上方 lcd_init 后提前启动,
     * 与开机图并行连接; 配网模式走 prov_start: AP + captive portal) */
    if (prov_is_active())
        xTaskCreate(prov_start,"prov",1024*4,NULL,15,NULL);

    uint32_t lt=0,fc=0;
    uint32_t wall_lt=0;
    while(1){
        vTaskDelay(pdMS_TO_TICKS(5));
        bl_wdt_feed();      /* 喂看门狗 */
        xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
        lv_timer_handler();

        /* 图播放: 时钟显示 wall_clock_sec 秒 ↔ 壁纸显示 wall_sec 秒, 各自独立
         * (配网模式不挂壁纸层, 跳过) */
        if (!prov_is_active() && g_cfg.wall_mode && g_cfg.wall_valid) {
            uint32_t t = lv_tick_get();
            if (wall_lt == 0) wall_lt = t;
            uint32_t dur = g_wall_vis ? (uint32_t)g_cfg.wall_sec
                                      : (uint32_t)g_cfg.wall_clock_sec;
            if (t - wall_lt >= dur * 1000) {
                wall_lt = t;
                g_wall_vis = !g_wall_vis;
                img_wall_set_vis(g_wall_vis);
            }
        } else if (g_wall_vis) {     /* 关闭/无壁纸: 恢复时钟 UI */
            g_wall_vis = 0;
            wall_lt = 0;
            img_wall_set_vis(0);
        }

        /* 更新 hour/minute 两个 textarea(仅当真正变化时;
         * SNTP 未同步时 g_time_str 是 "Connecting...", 保持 88:88 占位不覆盖) */
        clock_text_sync();

        /* point 冒号闪烁: 0.5s 亮 / 0.5s 灭(亮灭共 1 秒) */
        static int32_t prev_half = -1;
        if(g_time_str[4]=='-'){
            int32_t half = lv_tick_get() / 500;   /* 500ms 一格 */
            if(half != prev_half){
                prev_half = half;
                lv_obj_set_style_opa(guider_ui.screen.point,
                    (half % 2) == 0 ? LV_OPA_COVER : LV_OPA_TRANSP,
                    LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        }

        /* 日期区: moon=月 day=日 week=英文星期(Mon..Sun) week_num=星期数(周一=1..周日=7) */
        static char prev_d[6];
        if(g_time_str[4]=='-' && g_wday >= 0 && !img_wall_anim_busy()){
            char cur[6] = {g_time_str[5], g_time_str[6], g_time_str[8], g_time_str[9], (char)g_wday, 0};
            if(memcmp(cur, prev_d, 6)){
                memcpy(prev_d, cur, 6);
                static const char * const wname[7] =
                    {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                char buf[3];
                int mon = (g_time_str[5]-'0')*10 + (g_time_str[6]-'0');
                int day = (g_time_str[8]-'0')*10 + (g_time_str[9]-'0');
                snprintf(buf, sizeof(buf), "%d", mon);
                lv_textarea_set_text(guider_ui.screen.moon, buf);
                snprintf(buf, sizeof(buf), "%d", day);
                lv_textarea_set_text(guider_ui.screen.day, buf);
                lv_textarea_set_text(guider_ui.screen.week, wname[g_wday]);
                snprintf(buf, sizeof(buf), "%d", g_wday == 0 ? 7 : g_wday);
                lv_textarea_set_text(guider_ui.screen.week_num, buf);
            }
        }
        xSemaphoreGive(lvgl_mutex);
        fc++;
        if(lv_tick_get()-lt>=1000){ lt=lv_tick_get(); fc=0; }
    }
}

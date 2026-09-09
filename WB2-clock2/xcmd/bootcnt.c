/**
 * @brief bootcnt — 上电开机计数实现
 *
 * 存储位置: 媒体分区最后一个 4KB 扇区。
 *   media 分区 [0x1A2000, 0x1E9000) = 72 扇区;
 *   开机图 [0, 0x22600) + 壁纸 [0x22600, 0x40100) 之后全部空闲,
 *   上传工具只擦自己的区域, 不会碰到最后一个扇区。
 * 格式: 扇区头部 8 字节 = magic(4) + count(4), 小端。
 * bl_flash_erase/write 内部 GLOBAL_IRQ_SAVE/RESTORE + ROM 轮询驱动,
 * 不依赖中断/调度器, bfl_main 极早期 (UART 初始化后) 即可安全调用。
 */
#include <stdio.h>
#include <string.h>
#include <bl_flash.h>
#include <FreeRTOS.h>
#include <task.h>
#include "bootcnt.h"

/* 计数器扇区绝对地址: 分区表 media=0x1A2000, 尺寸 0x47000 (290KB) */
#define BOOTCNT_ADDR   (0x1A2000 + 0x46000)   /* = 0x1E8000, 媒体最后 1 个 4KB 扇区 */
#define BOOTCNT_SZ     0x1000
#define BOOTCNT_MAGIC  0xB007C0DEu

static int bootcnt_read(uint32_t *count)
{
    uint8_t b[8];
    uint32_t magic;

    if (bl_flash_read(BOOTCNT_ADDR, b, sizeof(b)) != 0) return -1;
    memcpy(&magic, b, 4);
    if (magic != BOOTCNT_MAGIC) {       /* 未初始化/已清除 */
        *count = 0;
        return 0;
    }
    memcpy(count, b + 4, 4);
    if (*count > 1000) *count = 0;      /* 异常值防御 */
    return 0;
}

int bootcnt_get(void)
{
    uint32_t count = 0;
    if (bootcnt_read(&count) != 0) return -1;
    return (int)count;
}

int bootcnt_clear(void)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (bl_flash_erase(BOOTCNT_ADDR, BOOTCNT_SZ) != 0) {
            continue;
        }
        if (bootcnt_get() == 0) {
            return 0;               /* 已清零并读回验证 */
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    /* erase 反复失败 (实测配网 GOT_IP 时刻 WiFi 双活时发生): NOR flash
     * 编程只能 1→0, 不清除 magic 直接写 0 也能清零 count —— magic 被擦掉
     * 时读侧视为 0, magic 还在时 count=0, 两路都保证计数归零 */
    {
        uint8_t z[4] = {0};
        if (bl_flash_write(BOOTCNT_ADDR + 4, z, sizeof(z)) != 0) {
            return -1;
        }
    }
    return (bootcnt_get() == 0) ? 0 : -1;
}

void bootcnt_restore(uint32_t count)
{
    uint8_t b[8];
    uint32_t magic = BOOTCNT_MAGIC;

    if (count == 0) {
        bootcnt_clear();
        return;
    }
    if (bl_flash_erase(BOOTCNT_ADDR, BOOTCNT_SZ) != 0) return;
    memcpy(b, &magic, 4);
    memcpy(b + 4, &count, 4);
    bl_flash_write(BOOTCNT_ADDR, b, sizeof(b));
}

int bootcnt_inc(void)
{
    uint32_t count = 0;
    uint32_t magic = BOOTCNT_MAGIC;
    uint8_t b[8];

    if (bootcnt_read(&count) != 0) return -1;
    count++;
    if (count > 1000) count = 1;        /* 防异常增长(正常会被 keepalive/SNTP 清零) */
    if (bl_flash_erase(BOOTCNT_ADDR, BOOTCNT_SZ) != 0) return -1;
    memcpy(b, &magic, 4);
    memcpy(b + 4, &count, 4);
    if (bl_flash_write(BOOTCNT_ADDR, b, sizeof(b)) != 0) return -1;
    return (int)count;
}

/* ============ bfl_main 极早期钩子 ============
 * SDK bfl_main.c 在串口初始化后、"Booting Ai-WB2 Modules..." 前调用
 * (弱符号, 其他工程不定义则无操作)。这里完成"上电第一时间计数",
 * main() 里 prov_boot_check 直接用结果, 无需再等 easyflash/LCD。 */
int g_boot_count = -1;   /* -1 = 未计数; main 里兜底重试 */

void app_boot_early(void)
{
    g_boot_count = bootcnt_inc();
    /* WB2-clock2: 打印并入横幅 (boot: %u / 5), 这里只保留计数 */
    /* printf("[PROV] early boot count=%d\r\n", g_boot_count); */
}

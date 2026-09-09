/**
 * @brief bootcnt — 上电开机计数 (连续按 EN 5 次进配网)
 *
 * 计数存媒体分区最后一个 4KB 扇区 (flash 0x1E8000), 用 bl_flash 直接读写,
 * 绕过 easyflash —— 可在 bfl_main 极早期调用 (bl_flash_init 由
 * bl_sys_early_init 完成, 无需调度器/easyflash), 实现"上电第一时间计数"。
 * 出厂重置(整区擦除媒体)会清掉计数, 由 factory_reset 读回恢复。
 */
#ifndef BOOTCNT_H
#define BOOTCNT_H

#include <stdint.h>

int  bootcnt_get(void);                 /* 只读当前计数 (0=未初始化) */
int  bootcnt_inc(void);                 /* 计数+1 并写回, 返回新值; -1=失败 */
int  bootcnt_clear(void);               /* 清零 (配网成功/稳定运行/显示时间时); 0=已验证清零 */
void bootcnt_restore(uint32_t count);   /* 写回指定计数 (出厂重置擦媒体后恢复) */

#endif /* BOOTCNT_H */

#
# WB2-clock2 — ST7789 172x320 LCD + LVGL + WiFi + SNTP
#

PROJECT_NAME := WB2-clock2
PROJECT_PATH := $(abspath .)
PROJECT_BOARD := evb
export PROJECT_PATH PROJECT_BOARD

-include ./proj_config.mk

# SDK 路径解析（发给别人时无需改代码）:
#   1. 环境变量 BL60X_SDK_PATH 优先
#   2. 否则探测上级目录是否是 SDK 树（项目放在 SDK 的 applications/ 下时生效）
#   3. 都找不到时用下面的兜底路径（本机开发路径；别人用请改这一行或设环境变量）
ifndef BL60X_SDK_PATH
BL60X_SDK_PATH ?= $(abspath $(CURDIR)/../../..)
ifeq ("$(wildcard $(BL60X_SDK_PATH)/make_scripts_riscv/project.mk)","")
BL60X_SDK_PATH := /home/xemowo/Ai-Thinker-WB2
endif
endif

COMPONENTS_NETWORK := sntp dns_server
COMPONENTS_BLSYS   := bltime blfdt blmtd blota bloop loopadc looprt loopset
COMPONENTS_VFS     := romfs
COMPONENTS_LVGL    := lvgl lv_srcs draw extra extra_themes extra_layouts extra_libs extra_others extra_widgets

INCLUDE_COMPONENTS += freertos_riscv_ram bl602 bl602_std newlibc wifi wifi_manager wpa_supplicant bl_os_adapter wifi_hosal hosal mbedtls_lts lwip lwip_dhcpd vfs yloop utils cli blog blog_testc blcrypto_suite
INCLUDE_COMPONENTS += easyflash4 coredump rfparam_adapter_tmp
INCLUDE_COMPONENTS += $(COMPONENTS_NETWORK)
INCLUDE_COMPONENTS += $(COMPONENTS_BLSYS)
INCLUDE_COMPONENTS += $(COMPONENTS_VFS)
INCLUDE_COMPONENTS += $(COMPONENTS_LVGL)
INCLUDE_COMPONENTS += cjson
INCLUDE_COMPONENTS += $(PROJECT_NAME)

# mbedtls 每个 SSL 连接默认 in/out 各 16KB(共 32KB), aos 堆只剩 ~22.7KB 时
# mbedtls_ssl_setup 分配失败 -> 和风 HTTPS 失败。缩到 8KB/方向, 见 mbedtls_user_config.h
EXTRA_CFLAGS += -D MBEDTLS_USER_CONFIG_FILE=\"mbedtls_user_config.h\"
# NTP 重同步周期: 默认 1 小时 → 60 秒(离线走时校准需要较密的对时点)
EXTRA_CFLAGS += -D SNTP_UPDATE_DELAY=60000
# lwIP SNTP 加速(lwip/apps/sntp_opts.h 均为 #if !defined 保护, 可 -D 覆盖):
#   SNTP_STARTUP_DELAY=0   去掉 sntp_init 后随机 0~5s 首包延时(SNTP_STARTUP_DELAY_FUNC)
#   SNTP_RECV_TIMEOUT=3000 UDP 首包丢失时 15s 才重发 -> 3s 快速重发
#   SNTP_RETRY_TIMEOUT=3000 同上, 重试指数退避起点
#   SNTP_MAX_SERVERS=2    默认只有 1 个槽位, 开 2 个做 IP/域名双兜底
EXTRA_CFLAGS += -D SNTP_STARTUP_DELAY=0
EXTRA_CFLAGS += -D SNTP_RECV_TIMEOUT=3000
EXTRA_CFLAGS += -D SNTP_RETRY_TIMEOUT=3000
EXTRA_CFLAGS += -D SNTP_MAX_SERVERS=2
# 上电不打印 SDK 启动信息: blog INFO + bfl_main puts + boot2 分区表 + easyflash INFO。
# 有用的信息(版本/boot 计数等)已挪进 main.c 的 boot_banner_show()。
EXTRA_CFLAGS += -D SYS_BOOT_LOG_DISABLE

# 上电横幅 git 信息(编译期注入; 非 git 树时回退 unknown):
#   FW_GIT_HASH=短哈希  FW_GIT_BRANCH=分支名  FW_GIT_DIRTY=0/1(工作区有无改动)
FW_GIT_HASH   := $(shell git -C $(PROJECT_PATH) rev-parse --short HEAD 2>/dev/null || echo unknown)
FW_GIT_BRANCH := $(shell git -C $(PROJECT_PATH) rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
FW_GIT_DIRTY  := $(shell git -C $(PROJECT_PATH) diff --quiet 2>/dev/null; echo $$?)
EXTRA_CFLAGS += -D FW_GIT_HASH=\"$(FW_GIT_HASH)\" -D FW_GIT_BRANCH=\"$(FW_GIT_BRANCH)\" -D FW_GIT_DIRTY=$(FW_GIT_DIRTY)

include $(BL60X_SDK_PATH)/make_scripts_riscv/project.mk

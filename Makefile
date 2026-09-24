#
# WB2-clock2 — ST7789 172x320 LCD + LVGL + WiFi + SNTP
#

PROJECT_NAME := WB2-clock2
PROJECT_PATH := $(abspath .)
PROJECT_BOARD := evb
export PROJECT_PATH PROJECT_BOARD

-include ./proj_config.mk

# SDK 路径解析（仓库自带 sdk/, clone 下来直接能编）:
#   1. 环境变量 BL60X_SDK_PATH 优先
#   2. 仓库内置:   <仓库根>/sdk
#   3. 上级目录:   项目被放进 SDK 的 applications/ 下时生效
#   4. 都找不到就报错 —— 不再静默指向某台机器的绝对路径, 拖到链接阶段才炸
ifeq ($(origin BL60X_SDK_PATH),undefined)
BL60X_SDK_PATH := $(firstword $(foreach d,$(PROJECT_PATH)/sdk $(abspath $(CURDIR)/../../..),$(if $(wildcard $(d)/make_scripts_riscv/project.mk),$(d))))
endif

ifeq ("$(wildcard $(BL60X_SDK_PATH)/make_scripts_riscv/project.mk)","")
$(error 找不到可用的 WB2 SDK。若仓库自带的 sdk/ 还在, 先拉子模块: git submodule update --init --recursive ；否则用 BL60X_SDK_PATH=<你的 SDK 路径> 指定)
endif

# 工具链解析 —— 与 flash.sh 共用 tools/find_toolchain.sh, 两边口径完全一致:
#   本机已经装了能编 rv32imfc/ilp32f 的 riscv64-unknown-elf 就直接用, 一个字节都不下载;
#   没有才落到仓库自带的 submodule (sdk/toolchain/riscv/<平台>)。
#   顺序: $CONFIG_TOOLPREFIX / $BL60X_TOOLCHAIN_PATH > PATH > /opt/riscv 等常见目录 > 仓库自带
#
# 前面那三个赋值是把 make 变量显式喂给脚本 —— 命令行上传的 make 变量(make FOO=bar)
# 不会进环境, 只写 sh tools/find_toolchain.sh 的话脚本根本看不见它们。
#
# 必须赶在 include project.mk 之前定下来: 那边 toolchain.mk 写的是 `CONFIG_TOOLPREFIX ?=`,
# 这里给了值它就不再覆盖, 后面 CC/LD/OBJCOPY 也就跟着指到这份工具链上了。
ifeq ($(origin CONFIG_TOOLPREFIX),undefined)
CONFIG_TOOLPREFIX := $(shell CONFIG_TOOLPREFIX="$(CONFIG_TOOLPREFIX)" BL60X_TOOLCHAIN_PATH="$(BL60X_TOOLCHAIN_PATH)" BL60X_SDK_PATH="$(BL60X_SDK_PATH)" sh $(PROJECT_PATH)/tools/find_toolchain.sh)
endif
ifeq ($(strip $(CONFIG_TOOLPREFIX)),)
$(error 没找到能用的 riscv64-unknown-elf 工具链(上面 find_toolchain 说了查到哪、为什么不行)。直接跑 ./flash.sh 会自动拉仓库自带的那份(首次约 2.3GB, 只需一次); 本机已装的话也可以 BL60X_TOOLCHAIN_PATH=<工具链目录> make, 或 make CONFIG_TOOLPREFIX=<前缀>)
endif
export CONFIG_TOOLPREFIX

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

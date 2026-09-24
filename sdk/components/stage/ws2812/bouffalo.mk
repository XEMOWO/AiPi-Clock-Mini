#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)
ifeq ($(strip $(CONFIG_WS2812_MODE)),)
  # 变量未定义或为空（包括仅含空格的情况）
  $(info CONFIG_WS2812_MODE is not set or empty)
else

 $(info CONFIG_WS2812_MODE is set)
ifeq ($(CONFIG_WS2812_MODE),SPI_MODE)
CFLAGS += -DWS281X_SPI_MODE
endif

ifeq ($(CONFIG_WS2812_MODE),IR_MODE)
CFLAGS += -DWS281X_IR_MODE
endif

COMPONENT_ADD_INCLUDEDIRS := ./

COMPONENT_OBJS := $(patsubst %.c,%.o, $(COMPONENT_SRCS))

COMPONENT_SRCDIRS := ./
endif
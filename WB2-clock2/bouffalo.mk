include $(BL60X_SDK_PATH)/components/network/ble/ble_common.mk

ifeq ($(CONFIG_ENABLE_PSM_RAM),1)
CPPFLAGS += -DCONF_USER_ENABLE_PSRAM
endif

# GUI Guider generated code
COMPONENT_ADD_INCLUDEDIRS += . generated generated/screens generated/events custom assets/fonts assets/images xcmd
COMPONENT_SRCDIRS += generated generated/screens generated/events generated/assets/images custom xcmd

# Remove the gg_utils.c which uses platform-specific APIs we don't have
# We'll use our own guider_ui definition in main.c

# Spatial Decoder Library - Android.mk for Android Build System
# This file should be placed in vendor/mediatek/proprietary/hardware/audio/spatial_decoder/Android.mk
# or equivalent location in the MT5862 vendor tree

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := libspatialdecoder
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_OWNER := mediatek

# Source files
LOCAL_SRC_FILES := \
    src/decoder/spatial_decoder.c \
    src/pcm/spatial_pcm.c \
    src/pcm/spatial_channel_layout.c \
    src/config/spatial_config.c \
    src/config/property_bridge.c \
    src/downmix/spatial_downmix.c

# Include directories
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/../include \
    $(LOCAL_PATH)/src/decoder \
    $(LOCAL_PATH)/src/pcm \
    $(LOCAL_PATH)/src/config \
    $(LOCAL_PATH)/src/downmix

# Compiler flags
LOCAL_CFLAGS := -std=c11 -Wall -Wextra -Werror
LOCAL_CFLAGS += -DANDROID -DANDROID_SMP=1
LOCAL_CFLAGS += -DANDROID_BUILD -DANDROID_API=28
LOCAL_CFLAGS += -march=armv7-a -mthumb -mfpu=neon

# Link against FFmpeg libraries (must be prebuilt or built separately)
LOCAL_SHARED_LIBRARIES := \
    libavcodec \
    libavutil \
    libavformat \
    libswresample \
    liblog \
    libcutils \
    libutils \
    libdl

# Compiler flags for optimization
LOCAL_CFLAGS += -O2 -fPIC
LOCAL_CFLAGS += -fvisibility=hidden

# Version info
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_OWNER := mediatek

include $(BUILD_SHARED_LIBRARY)
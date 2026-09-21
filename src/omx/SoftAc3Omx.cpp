#define LOG_TAG "SoftAc3Omx"
#include "spatial_android_compat.h"

#include "SoftAc3Omx.h"

#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/hexdump.h>

#include <media/stagefright/omx/OMXComponent.h>

#include <media/MediaCodecList.h>
#include <media/MediaCodecInfo.h>
#include <media/MediaCodec.h>

namespace android {

// Helper function to map MIME type to spatial_codec_t
static spatial_codec_t mimeToSpatialCodec(const char* mime) {
    if (!mime) return SPATIAL_CODEC_AC3;
    
    if (strcmp(mime, "audio/ac3") == 0) return SPATIAL_CODEC_AC3;
    if (strcmp(mime, "audio/eac3") == 0) return SPATIAL_CODEC_EAC3;
    if (strcmp(mime, "audio/dts") == 0) return SPATIAL_CODEC_DTS;
    if (strcmp(mime, "audio/vnd.dolby.mlp") == 0) return SPATIAL_CODEC_TRUEHD;
    if (strcmp(mime, "audio/truehd") == 0) return SPATIAL_CODEC_TRUEHD;
    
    return SPATIAL_CODEC_AC3; // Default fallback
}

// Helper function to get MIME type from component role
static const char* getMimeTypeFromRole() {
    // This would typically be implemented by getting the component role
    // For now, we'll use a simple approach - in a real implementation,
    // this would query the OMX component role
    // For now, we'll default to E-AC3 as it's the most common for Android TV
    return "audio/eac3";
}

namespace android {

// Helper function to check debug property
static bool isDebugEnabled() {
    char value[PROPERTY_VALUE_MAX];
    return __system_property_get("persist.vendor.spatialdm.debug", value) > 0 && atoi(value) > 0;
}

// Helper function to log downmix config matrix
static void logDownmixConfig(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    if (config->layout == SPATIAL_LAYOUT_5_1) {
        ALOGI("%s: 5.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
              config->gains_5_1.left, config->gains_5_1.right,
              config->gains_5_1.center, config->gains_5_1.surround,
              config->gains_5_1.lfe);
    } else if (config->layout == SPATIAL_LAYOUT_7_1) {
        ALOGI("%s: 7.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
              config->gains_7_1.left, config->gains_7_1.right,
              config->gains_7_1.center, config->gains_7_1.side,
              config->gains_7_1.rear, config->gains_7_1.lfe);
    }
}

// Track previous config for change detection
static spatial_downmix_config_t g_prev_downmix_config = {0};
static bool g_first_config_log = true;

static void logDownmixConfigIfChanged(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    // Check if config has changed since last log
    bool changed = g_first_config_log ||
                   memcmp(&g_prev_downmix_config, config, sizeof(spatial_downmix_config_t)) != 0;
    
    if (changed) {
        if (config->layout == SPATIAL_LAYOUT_5_1) {
            ALOGI("%s: 5.1 downmix matrix:", tag);
            ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
                  config->gains_5_1.left, config->gains_5_1.right,
                  config->gains_5_1.center, config->gains_5_1.surround,
                  config->gains_5_1.lfe);
        } else if (config->layout == SPATIAL_LAYOUT_7_1) {
            ALOGI("%s: 7.1 downmix matrix:", tag);
            ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
                  config->gains_7_1.left, config->gains_7_1.right,
                  config->gains_7_1.center, config->gains_7_1.side,
                  config->gains_7_1.rear, config->gains_7_1.lfe);
        }
        
        // Update previous config
        memcpy(&g_prev_downmix_config, config, sizeof(spatial_downmix_config_t));
        g_first_config_log = false;
    }
}

namespace android {

// Helper function to map MIME type to spatial_codec_t
static spatial_codec_t mimeToSpatialCodec(const char* mime) {
    if (!mime) return SPATIAL_CODEC_AC3;
    
    if (strcmp(mime, "audio/ac3") == 0) return SPATIAL_CODEC_AC3;
    if (strcmp(mime, "audio/eac3") == 0) return SPATIAL_CODEC_EAC3;
    if (strcmp(mime, "audio/dts") == 0) return SPATIAL_CODEC_DTS;
    if (strcmp(mime, "audio/vnd.dolby.mlp") == 0) return SPATIAL_CODEC_TRUEHD;
    if (strcmp(mime, "audio/truehd") == 0) return SPATIAL_CODEC_TRUEHD;
    
    return SPATIAL_CODEC_AC3; // Default fallback
}

// Helper function to get MIME type from component role
static const char* getMimeTypeFromRole() {
    // This would typically be implemented by getting the component role
    // For now, we'll use a simple approach - in a real implementation,
    // this would query the OMX component role
    // For now, we'll default to E-AC3 as it's the most common for Android TV
    return "audio/eac3";
}

// Helper function to check debug property
static bool isDebugEnabled() {
    char value[PROPERTY_VALUE_MAX];
    return __system_property_get("persist.vendor.spatialdm.debug", value) > 0 && atoi(value) > 0;
}

// Helper function to log downmix config matrix
static void logDownmixConfig(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    if (config->layout == SPATIAL_LAYOUT_5_1) {
        ALOGI("%s: 5.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
              config->gains_5_1.left, config->gains_5_1.right,
              config->gains_5_1.center, config->gains_5_1.surround,
              config->gains_5_1.lfe);
    } else if (config->layout == SPATIAL_LAYOUT_7_1) {
        ALOGI("%s: 7.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
              config->gains_7_1.left, config->gains_7_1.right,
              config->gains_7_1.center, config->gains_7_1.side,
              config->gains_7_1.rear, config->gains_7_1.lfe);
    }
}

namespace android {

// Helper function to map MIME type to spatial_codec_t
static spatial_codec_t mimeToSpatialCodec(const char* mime) {
    if (!mime) return SPATIAL_CODEC_AC3;
    
    if (strcmp(mime, "audio/ac3") == 0) return SPATIAL_CODEC_AC3;
    if (strcmp(mime, "audio/eac3") == 0) return SPATIAL_CODEC_EAC3;
    if (strcmp(mime, "audio/dts") == 0) return SPATIAL_CODEC_DTS;
    if (strcmp(mime, "audio/vnd.dolby.mlp") == 0) return SPATIAL_CODEC_TRUEHD;
    if (strcmp(mime, "audio/truehd") == 0) return SPATIAL_CODEC_TRUEHD;
    
    return SPATIAL_CODEC_AC3; // Default fallback
}

// Helper function to get MIME type from component role
static const char* getMimeTypeFromRole() {
    // This would typically be implemented by getting the component role
    // For now, we'll use a simple approach - in a real implementation,
    // this would query the OMX component role
    // For now, we'll default to E-AC3 as it's the most common for Android TV
    return "audio/eac3";
}

// Helper function to check debug property
static bool isDebugEnabled() {
    char value[PROPERTY_VALUE_MAX];
    return __system_property_get("persist.vendor.spatialdm.debug", value) > 0 && atoi(value) > 0;
}

// Helper function to log downmix config matrix
static void logDownmixConfig(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    if (config->layout == SPATIAL_LAYOUT_5_1) {
        ALOGI("%s: 5.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
              config->gains_5_1.left, config->gains_5_1.right,
              config->gains_5_1.center, config->gains_5_1.surround,
              config->gains_5_1.lfe);
    } else if (config->layout == SPATIAL_LAYOUT_7_1) {
        ALOGI("%s: 7.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
              config->gains_7_1.left, config->gains_7_1.right,
              config->gains_7_1.center, config->gains_7_1.side,
              config->gains_7_1.rear, config->gains_7_1.lfe);
    }
}

// Track previous config for change detection
static spatial_downmix_config_t g_prev_downmix_config = {0};
static bool g_first_config_log = true;

static void logDownmixConfigIfChanged(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    // Check if config has changed since last log
    bool changed = g_first_config_log ||
                   memcmp(&g_prev_downmix_config, config, sizeof(spatial_downmix_config_t)) != 0;
    
    if (changed) {
        if (config->layout == SPATIAL_LAYOUT_5_1) {
            ALOGI("%s: 5.1 downmix matrix:", tag);
            ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
                  config->gains_5_1.left, config->gains_5_1.right,
                  config->gains_5_1.center, config->gains_5_1.surround,
                  config->gains_5_1.lfe);
        } else if (config->layout == SPATIAL_LAYOUT_7_1) {
            ALOGI("%s: 7.1 downmix matrix:", tag);
            ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
                  config->gains_7_1.left, config->gains_7_1.right,
                  config->gains_7_1.center, config->gains_7_1.side,
                  config->gains_7_1.rear, config->gains_7_1.lfe);
        }
        
        // Update previous config
        memcpy(&g_prev_downmix_config, config, sizeof(spatial_downmix_config_t));
        g_first_config_log = false;
    }
}

namespace android {

// Helper function to map MIME type to spatial_codec_t
static spatial_codec_t mimeToSpatialCodec(const char* mime) {
    if (!mime) return SPATIAL_CODEC_AC3;
    
    if (strcmp(mime, "audio/ac3") == 0) return SPATIAL_CODEC_AC3;
    if (strcmp(mime, "audio/eac3") == 0) return SPATIAL_CODEC_EAC3;
    if (strcmp(mime, "audio/dts") == 0) return SPATIAL_CODEC_DTS;
    if (strcmp(mime, "audio/vnd.dolby.mlp") == 0) return SPATIAL_CODEC_TRUEHD;
    if (strcmp(mime, "audio/truehd") == 0) return SPATIAL_CODEC_TRUEHD;
    
    return SPATIAL_CODEC_AC3; // Default fallback
}

// Helper function to get MIME type from component role
static const char* getMimeTypeFromRole() {
    // This would typically be implemented by getting the component role
    // For now, we'll use a simple approach - in a real implementation,
    // this would query the OMX component role
    // For now, we'll default to E-AC3 as it's the most common for Android TV
    return "audio/eac3";
}

// Helper function to check debug property
static bool isDebugEnabled() {
    char value[PROPERTY_VALUE_MAX];
    return __system_property_get("persist.vendor.spatialdm.debug", value) > 0 && atoi(value) > 0;
}

// Helper function to log downmix config matrix
static void logDownmixConfig(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    if (config->layout == SPATIAL_LAYOUT_5_1) {
        ALOGI("%s: 5.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
              config->gains_5_1.left, config->gains_5_1.right,
              config->gains_5_1.center, config->gains_5_1.surround,
              config->gains_5_1.lfe);
    } else if (config->layout == SPATIAL_LAYOUT_7_1) {
        ALOGI("%s: 7.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
              config->gains_7_1.left, config->gains_7_1.right,
              config->gains_7_1.center, config->gains_7_1.side,
              config->gains_7_1.rear, config->gains_7_1.lfe);
    }
}

// Track previous config for change detection
static spatial_downmix_config_t g_prev_downmix_config = {0};
static bool g_first_config_log = true;

static void logDownmixConfigIfChanged(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    // Check if config has changed since last log
    bool changed = g_first_config_log ||
                   memcmp(&g_prev_downmix_config, config, sizeof(spatial_downmix_config_t)) != 0;
    
    if (changed) {
        if (config->layout == SPATIAL_LAYOUT_5_1) {
            ALOGI("%s: 5.1 downmix matrix:", tag);
            ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
                  config->gains_5_1.left, config->gains_5_1.right,
                  config->gains_5_1.center, config->gains_5_1.surround,
                  config->gains_5_1.lfe);
        } else if (config->layout == SPATIAL_LAYOUT_7_1) {
            ALOGI("%s: 7.1 downmix matrix:", tag);
            ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
                  config->gains_7_1.left, config->gains_7_1.right,
                  config->gains_7_1.center, config->gains_7_1.side,
                  config->gains_7_1.rear, config->gains_7_1.lfe);
        }
        
        // Update previous config
        memcpy(&g_prev_downmix_config, config, sizeof(spatial_downmix_config_t));
        g_first_config_log = false;
    }
}

namespace android {

// Helper function to map MIME type to spatial_codec_t
static spatial_codec_t mimeToSpatialCodec(const char* mime) {
    if (!mime) return SPATIAL_CODEC_AC3;
    
    if (strcmp(mime, "audio/ac3") == 0) return SPATIAL_CODEC_AC3;
    if (strcmp(mime, "audio/eac3") == 0) return SPATIAL_CODEC_EAC3;
    if (strcmp(mime, "audio/dts") == 0) return SPATIAL_CODEC_DTS;
    if (strcmp(mime, "audio/vnd.dolby.mlp") == 0) return SPATIAL_CODEC_TRUEHD;
    if (strcmp(mime, "audio/truehd") == 0) return SPATIAL_CODEC_TRUEHD;
    
    return SPATIAL_CODEC_AC3; // Default fallback
}

// Helper function to get MIME type from component role
static const char* getMimeTypeFromRole() {
    // This would typically be implemented by getting the component role
    // For now, we'll use a simple approach - in a real implementation,
    // this would query the OMX component role
    // For now, we'll default to E-AC3 as it's the most common for Android TV
    return "audio/eac3";
}

// Helper function to check debug property
static bool isDebugEnabled() {
    char value[PROPERTY_VALUE_MAX];
    return __system_property_get("persist.vendor.spatialdm.debug", value) > 0 && atoi(value) > 0;
}

// Helper function to log downmix config matrix
static void logDownmixConfig(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    if (config->layout == SPATIAL_LAYOUT_5_1) {
        ALOGI("%s: 5.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
              config->gains_5_1.left, config->gains_5_1.right,
              config->gains_5_1.center, config->gains_5_1.surround,
              config->gains_5_1.lfe);
    } else if (config->layout == SPATIAL_LAYOUT_7_1) {
        ALOGI("%s: 7.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
              config->gains_7_1.left, config->gains_7_1.right,
              config->gains_7_1.center, config->gains_7_1.side,
              config->gains_7_1.rear, config->gains_7_1.lfe);
    }
}

// Track previous config for change detection
static spatial_downmix_config_t g_prev_downmix_config = {0};
static bool g_first_config_log = true;

static void logDownmixConfigIfChanged(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    // Check if config has changed since last log
    bool changed = g_first_config_log ||
                   memcmp(&g_prev_downmix_config, config, sizeof(spatial_downmix_config_t)) != 0;
    
    if (changed) {
        if (config->layout == SPATIAL_LAYOUT_5_1) {
            ALOGI("%s: 5.1 downmix matrix:", tag);
            ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
                  config->gains_5_1.left, config->gains_5_1.right,
                  config->gains_5_1.center, config->gains_5_1.surround,
                  config->gains_5_1.lfe);
        } else if (config->layout == SPATIAL_LAYOUT_7_1) {
            ALOGI("%s: 7.1 downmix matrix:", tag);
            ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
                  config->gains_7_1.left, config->gains_7_1.right,
                  config->gains_7_1.center, config->gains_7_1.side,
                  config->gains_7_1.rear, config->gains_7_1.lfe);
        }
        
        // Update previous config
        memcpy(&g_prev_downmix_config, config, sizeof(spatial_downmix_config_t));
        g_first_config_log = false;
    }
}

namespace android {

// Helper function to map MIME type to spatial_codec_t
static spatial_codec_t mimeToSpatialCodec(const char* mime) {
    if (!mime) return SPATIAL_CODEC_AC3;
    
    if (strcmp(mime, "audio/ac3") == 0) return SPATIAL_CODEC_AC3;
    if (strcmp(mime, "audio/eac3") == 0) return SPATIAL_CODEC_EAC3;
    if (strcmp(mime, "audio/dts") == 0) return SPATIAL_CODEC_DTS;
    if (strcmp(mime, "audio/vnd.dolby.mlp") == 0) return SPATIAL_CODEC_TRUEHD.
    if (strcmp(mime, "audio/truehd") == 0) return SPATIAL_CODEC_TRUEHD;
    
    return SPATIAL_CODEC_AC3; // Default fallback
}

// Helper function to get MIME type from component role
static const char* getMimeTypeFromRole() {
    // This would typically be implemented by getting the component role
    // For now, we'll use a simple approach - in a real implementation,
    // this would query the OMX component role
    // For now, we'll default to E-AC3 as it's the most common for Android TV
    return "audio/eac3";
}

// Helper function to check debug property
static bool isDebugEnabled() {
    char value[PROPERTY_VALUE_MAX];
    return __system_property_get("persist.vendor.spatialdm.debug", value) > 0 && atoi(value) > 0;
}

// Helper function to log downmix config matrix
static void logDownmixConfig(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    if (config->layout == SPATIAL_LAYOUT_5_1) {
        ALOGI("%s: 5.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
              config->gains_5_1.left, config->gains_5_1.right,
              config->gains_5_1.center, config->gains_5_1.surround,
              config->gains_5_1.lfe);
    } else if (config->layout == SPATIAL_LAYOUT_7_1) {
        ALOGI("%s: 7.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
              config->gains_7_1.left, config->gains_7_1.right,
              config->gains_7_1.center, config->gains_7_1.side,
              config->gains_7_1.rear, config->gains_7_1.lfe);
    }
}

// Track previous config for change detection
static spatial_downmix_config_t g_prev_downmix_config = {0};
static bool g_first_config_log = true;

static void logDownmixConfigIfChanged(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    // Check if config has changed since last log
    bool changed = g_first_config_log ||
                   memcmp(&g_prev_downmix_config, config, sizeof(spatial_downmix_config_t)) != 0;
    
    if (changed) {
        if (config->layout == SPATIAL_LAYOUT_5_1) {
            ALOGI("%s: 5.1 downmix matrix:", tag);
            ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
                  config->gains_5_1.left, config->gains_5_1.right,
                  config->gains_5_1.center, config->gains_5_1.surround,
                  config->gains_5_1.lfe);
        } else if (config->layout == SPATIAL_LAYOUT_7_1) {
            ALOGI("%s: 7.1 downmix matrix:", tag);
            ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
                  config->gains_7_1.left, config->gains_7_1.right,
                  config->gains_7_1.center, config->gains_7_1.side,
                  config->gains_7_1.rear, config->gains_7_1.lfe);
        }
        
        // Update previous config
        memcpy(&g_prev_downmix_config, config, sizeof(spatial_downmix_config_t));
        g_first_config_log = false;
    }
}

namespace android {
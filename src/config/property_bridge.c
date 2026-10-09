#include "property_bridge.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define LOG_TAG "spatial_property"
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

#define PROP_PREFIX "persist.vendor.spatialdm."

#define PROP_LAYOUT "persist.vendor.spatialdm.layout"
#define PROP_DEBUG "persist.vendor.spatialdm.debug"

#define PROP_5_1_LEFT "persist.vendor.spatialdm.5_1.left"
#define PROP_5_1_RIGHT "persist.vendor.spatialdm.5_1.right"
#define PROP_5_1_CENTER "persist.vendor.spatialdm.5_1.center"
#define PROP_5_1_SURROUND "persist.vendor.spatialdm.5_1.surround"
#define PROP_5_1_LFE "persist.vendor.spatialdm.5_1.lfe"

// Fine-grained per-channel overrides (each controls exactly one slot;
// when set and valid they win over the coarse property above).
// NOTE: "center" and "lfe" keys are shared with the coarse set above.
#define PROP_5_1_FRONT_LEFT "persist.vendor.spatialdm.5_1.front_left"
#define PROP_5_1_FRONT_RIGHT "persist.vendor.spatialdm.5_1.front_right"
#define PROP_5_1_SURROUND_LEFT "persist.vendor.spatialdm.5_1.surround_left"
#define PROP_5_1_SURROUND_RIGHT "persist.vendor.spatialdm.5_1.surround_right"

#define PROP_7_1_LEFT "persist.vendor.spatialdm.7_1.left"
#define PROP_7_1_RIGHT "persist.vendor.spatialdm.7_1.right"
#define PROP_7_1_CENTER "persist.vendor.spatialdm.7_1.center"
#define PROP_7_1_SIDE "persist.vendor.spatialdm.7_1.side"
#define PROP_7_1_REAR "persist.vendor.spatialdm.7_1.rear"
#define PROP_7_1_LFE "persist.vendor.spatialdm.7_1.lfe"

// Fine-grained per-channel overrides (each controls exactly one slot;
// when set and valid they win over the coarse property above).
// NOTE: "center" and "lfe" keys are shared with the coarse set above.
#define PROP_7_1_FRONT_LEFT "persist.vendor.spatialdm.7_1.front_left"
#define PROP_7_1_FRONT_RIGHT "persist.vendor.spatialdm.7_1.front_right"
#define PROP_7_1_SIDE_LEFT "persist.vendor.spatialdm.7_1.side_left"
#define PROP_7_1_SIDE_RIGHT "persist.vendor.spatialdm.7_1.side_right"
#define PROP_7_1_REAR_LEFT "persist.vendor.spatialdm.7_1.rear_left"
#define PROP_7_1_REAR_RIGHT "persist.vendor.spatialdm.7_1.rear_right"

#define PROP_MATRIX_OBA "persist.vendor.spatialdm.matrix_encoding.oba"
#define PROP_MATRIX_CBA "persist.vendor.spatialdm.matrix_encoding.cba"

#define PROP_DRC "persist.vendor.spatialdm.drc"

#define MAX_PROP_VALUE 92

struct spatial_property_ctx {
    bool initialized;
};

// Coarse properties write one or two slots (shared pair gains).
// slots entries: {first, second}; second == -1 means single slot.
static const char* coarse_5_1_names[5] = {
    PROP_5_1_LEFT,
    PROP_5_1_RIGHT,
    PROP_5_1_CENTER,
    PROP_5_1_SURROUND,
    PROP_5_1_LFE
};
static const int coarse_5_1_slots[5][2] = {
    {0, -1}, {1, -1}, {2, -1}, {3, 4}, {5, -1},
};

// Fine properties override exactly one slot each (read after coarse).
static const char* fine_5_1_names[6] = {
    PROP_5_1_FRONT_LEFT,
    PROP_5_1_FRONT_RIGHT,
    PROP_5_1_CENTER,
    PROP_5_1_SURROUND_LEFT,
    PROP_5_1_SURROUND_RIGHT,
    PROP_5_1_LFE
};
static const int fine_5_1_slots[6] = {0, 1, 2, 3, 4, 5};

static const char* coarse_7_1_names[6] = {
    PROP_7_1_LEFT,
    PROP_7_1_RIGHT,
    PROP_7_1_CENTER,
    PROP_7_1_SIDE,
    PROP_7_1_REAR,
    PROP_7_1_LFE
};
static const int coarse_7_1_slots[6][2] = {
    {0, -1}, {1, -1}, {2, -1}, {3, 4}, {5, 6}, {7, -1},
};

static const char* fine_7_1_names[8] = {
    PROP_7_1_FRONT_LEFT,
    PROP_7_1_FRONT_RIGHT,
    PROP_7_1_CENTER,
    PROP_7_1_SIDE_LEFT,
    PROP_7_1_SIDE_RIGHT,
    PROP_7_1_REAR_LEFT,
    PROP_7_1_REAR_RIGHT,
    PROP_7_1_LFE
};
static const int fine_7_1_slots[8] = {0, 1, 2, 3, 4, 5, 6, 7};

static bool read_prop_float(const char* key, float* out_value) {
    char value[MAX_PROP_VALUE];
    int len = __system_property_get(key, value);
    if (len <= 0) return false;
    
    char* endptr;
    float val = strtof(value, &endptr);
    if (endptr == value || *endptr != '\0') {
        ALOGW("Property '%s' parse failed: invalid float format '%s', using default", key, value);
        return false;
    }
    if (isnan(val) || isinf(val)) {
        ALOGW("Property '%s' parse failed: invalid float value '%s', using default", key, value);
        return false;
    }
    
    *out_value = val;
    return true;
}

static bool read_prop_int(const char* key, int* out_value) {
    char value[MAX_PROP_VALUE];
    int len = __system_property_get(key, value);
    if (len <= 0) return false;
    
    char* endptr;
    long val = strtol(value, &endptr, 10);
    if (endptr == value || *endptr != '\0') {
        ALOGW("Property '%s' parse failed: invalid integer format '%s', using default", key, value);
        return false;
    }
    
    *out_value = (int)val;
    return true;
}

static bool read_prop_string(const char* key, char* out_value, size_t max_len) {
    int len = __system_property_get(key, out_value);
    return len > 0 && (size_t)len < max_len;
}

spatial_property_ctx_t* spatial_property_create(void) {
    spatial_property_ctx_t* ctx = calloc(1, sizeof(spatial_property_ctx_t));
    if (!ctx) return NULL;
    ctx->initialized = true;
    return ctx;
}

void spatial_property_destroy(spatial_property_ctx_t* ctx) {
    if (ctx) {
        free(ctx);
    }
}



bool spatial_property_validate_gain(float value) {
    return !isnan(value) && !isinf(value) && value >= 0.0f;
}

bool spatial_property_validate_layout(int value) {
    return value >= SPATIAL_LAYOUT_STEREO && value <= SPATIAL_LAYOUT_7_1;
}

bool spatial_property_validate_matrix_mode(int value) {
    return value >= SPATIAL_MATRIX_NONE && value <= SPATIAL_MATRIX_DPLII;
}

bool spatial_property_validate_content_type(int value) {
    return value >= SPATIAL_CONTENT_CBA && value <= SPATIAL_CONTENT_OBA;
}

int spatial_property_read_all(spatial_property_ctx_t* ctx, spatial_config_t* out_config) {
    if (!ctx || !out_config) return -1;
    
    spatial_config_get_defaults(out_config);
    
    int layout_val;
    if (read_prop_int(PROP_LAYOUT, &layout_val)) {
        if (spatial_property_validate_layout(layout_val)) {
            out_config->layout = (spatial_layout_t)layout_val;
        } else {
            ALOGW("Property '%s' validation failed: layout=%d (valid range: %d-%d), using default",
                  PROP_LAYOUT, layout_val, SPATIAL_LAYOUT_STEREO, SPATIAL_LAYOUT_7_1);
        }
    }
    
    int debug_val;
    if (read_prop_int(PROP_DEBUG, &debug_val)) {
        out_config->debug_enabled = (debug_val != 0);
    }
    
    float gain_val;
    int slot;
    // Coarse pass: shared pair gains fan out to both siblings.
    for (int i = 0; i < 5; i++) {
        if (read_prop_float(coarse_5_1_names[i], &gain_val)) {
            if (spatial_property_validate_gain(gain_val)) {
                out_config->gains_5_1[coarse_5_1_slots[i][0]] = gain_val;
                slot = coarse_5_1_slots[i][1];
                if (slot >= 0) {
                    out_config->gains_5_1[slot] = gain_val;
                }
            } else {
                ALOGW("Property '%s' validation failed: gain=%.3f (must be >= 0), using default",
                      coarse_5_1_names[i], gain_val);
            }
        }
    }
    // Fine pass: per-channel overrides win over coarse.
    for (int i = 0; i < 6; i++) {
        if (read_prop_float(fine_5_1_names[i], &gain_val)) {
            if (spatial_property_validate_gain(gain_val)) {
                out_config->gains_5_1[fine_5_1_slots[i]] = gain_val;
            } else {
                ALOGW("Property '%s' validation failed: gain=%.3f (must be >= 0), using default",
                      fine_5_1_names[i], gain_val);
            }
        }
    }

    for (int i = 0; i < 6; i++) {
        if (read_prop_float(coarse_7_1_names[i], &gain_val)) {
            if (spatial_property_validate_gain(gain_val)) {
                out_config->gains_7_1[coarse_7_1_slots[i][0]] = gain_val;
                slot = coarse_7_1_slots[i][1];
                if (slot >= 0) {
                    out_config->gains_7_1[slot] = gain_val;
                }
            } else {
                ALOGW("Property '%s' validation failed: gain=%.3f (must be >= 0), using default",
                      coarse_7_1_names[i], gain_val);
            }
        }
    }
    for (int i = 0; i < 8; i++) {
        if (read_prop_float(fine_7_1_names[i], &gain_val)) {
            if (spatial_property_validate_gain(gain_val)) {
                out_config->gains_7_1[fine_7_1_slots[i]] = gain_val;
            } else {
                ALOGW("Property '%s' validation failed: gain=%.3f (must be >= 0), using default",
                      fine_7_1_names[i], gain_val);
            }
        }
    }
    
    int matrix_val;
    if (read_prop_int(PROP_MATRIX_OBA, &matrix_val)) {
        if (spatial_property_validate_matrix_mode(matrix_val)) {
            out_config->matrix_oba = (spatial_matrix_mode_t)matrix_val;
        } else {
            ALOGW("Property '%s' validation failed: matrix_mode=%d (valid range: %d-%d), using default",
                  PROP_MATRIX_OBA, matrix_val, SPATIAL_MATRIX_NONE, SPATIAL_MATRIX_DPLII);
        }
    }
    if (read_prop_int(PROP_MATRIX_CBA, &matrix_val)) {
        if (spatial_property_validate_matrix_mode(matrix_val)) {
            out_config->matrix_cba = (spatial_matrix_mode_t)matrix_val;
        } else {
            ALOGW("Property '%s' validation failed: matrix_mode=%d (valid range: %d-%d), using default",
                  PROP_MATRIX_CBA, matrix_val, SPATIAL_MATRIX_NONE, SPATIAL_MATRIX_DPLII);
        }
    }
    
    char content_str[32];
    if (read_prop_string("persist.vendor.spatialdm.content_type", content_str, sizeof(content_str))) {
        if (strcmp(content_str, "oba") == 0) {
            out_config->content_type = SPATIAL_CONTENT_OBA;
        } else if (strcmp(content_str, "cba") == 0) {
            out_config->content_type = SPATIAL_CONTENT_CBA;
        } else {
            ALOGW("Property 'persist.vendor.spatialdm.content_type' invalid value '%s', using default", content_str);
        }
    }

    // Optional post-downmix DRC. Unset or unrecognized stays OFF, which
    // reproduces the linear downmix bit-identically.
    char drc_str[32];
    if (read_prop_string(PROP_DRC, drc_str, sizeof(drc_str))) {
        if (strcmp(drc_str, "off") == 0) {
            out_config->drc_mode = SPATIAL_DRC_OFF;
        } else if (strcmp(drc_str, "film") == 0) {
            out_config->drc_mode = SPATIAL_DRC_FILM;
        } else if (strcmp(drc_str, "night") == 0) {
            out_config->drc_mode = SPATIAL_DRC_NIGHT;
        } else {
            ALOGW("Property '%s' invalid value '%s', using default (off)",
                    PROP_DRC, drc_str);
        }
    }

    out_config->generation = 0;
    return 0;
}

int spatial_property_read_single(spatial_property_ctx_t* ctx, const char* key, float* out_value) {
    if (!ctx || !key || !out_value) return -1;
    return read_prop_float(key, out_value) ? 0 : -1;
}

int spatial_property_read_enum(spatial_property_ctx_t* ctx, const char* key, int* out_value) {
    if (!ctx || !key || !out_value) return -1;
    return read_prop_int(key, out_value) ? 0 : -1;
}
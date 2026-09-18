#include "property_bridge.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/system_properties.h>

#define PROP_PREFIX "persist.vendor.spatialdm."

#define PROP_LAYOUT "persist.vendor.spatialdm.layout"
#define PROP_DEBUG "persist.vendor.spatialdm.debug"

#define PROP_5_1_LEFT "persist.vendor.spatialdm.5_1.left"
#define PROP_5_1_RIGHT "persist.vendor.spatialdm.5_1.right"
#define PROP_5_1_CENTER "persist.vendor.spatialdm.5_1.center"
#define PROP_5_1_SURROUND "persist.vendor.spatialdm.5_1.surround"
#define PROP_5_1_LFE "persist.vendor.spatialdm.5_1.lfe"

#define PROP_7_1_LEFT "persist.vendor.spatialdm.7_1.left"
#define PROP_7_1_RIGHT "persist.vendor.spatialdm.7_1.right"
#define PROP_7_1_CENTER "persist.vendor.spatialdm.7_1.center"
#define PROP_7_1_SIDE "persist.vendor.spatialdm.7_1.side"
#define PROP_7_1_REAR "persist.vendor.spatialdm.7_1.rear"
#define PROP_7_1_LFE "persist.vendor.spatialdm.7_1.lfe"

#define PROP_MATRIX_OBA "persist.vendor.spatialdm.matrix_encoding.oba"
#define PROP_MATRIX_CBA "persist.vendor.spatialdm.matrix_encoding.cba"

#define MAX_PROP_VALUE 92

struct spatial_property_ctx {
    bool initialized;
};

static const char* gain_5_1_names[5] = {
    PROP_5_1_LEFT,
    PROP_5_1_RIGHT,
    PROP_5_1_CENTER,
    PROP_5_1_SURROUND,
    PROP_5_1_LFE
};

static const char* gain_7_1_names[6] = {
    PROP_7_1_LEFT,
    PROP_7_1_RIGHT,
    PROP_7_1_CENTER,
    PROP_7_1_SIDE,
    PROP_7_1_REAR,
    PROP_7_1_LFE
};

static bool read_prop_float(const char* key, float* out_value) {
    char value[MAX_PROP_VALUE];
    int len = __system_property_get(key, value);
    if (len <= 0) return false;
    
    char* endptr;
    float val = strtof(value, &endptr);
    if (endptr == value || *endptr != '\0') return false;
    if (isnan(val) || isinf(val)) return false;
    
    *out_value = val;
    return true;
}

static bool read_prop_int(const char* key, int* out_value) {
    char value[MAX_PROP_VALUE];
    int len = __system_property_get(key, value);
    if (len <= 0) return false;
    
    char* endptr;
    long val = strtol(value, &endptr, 10);
    if (endptr == value || *endptr != '\0') return false;
    
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

void spatial_config_get_defaults(spatial_config_t* config) {
    if (!config) return;
    
    memset(config, 0, sizeof(spatial_config_t));
    config->layout = SPATIAL_LAYOUT_5_1;
    
    config->gains_5_1.gains[SPATIAL_CH_FL] = 1.0f;
    config->gains_5_1.gains[SPATIAL_CH_FR] = 1.0f;
    config->gains_5_1.gains[SPATIAL_CH_FC] = 0.90f;
    config->gains_5_1.gains[SPATIAL_CH_LFE] = 0.25f;
    config->gains_5_1.gains[SPATIAL_CH_SL] = 0.55f;
    config->gains_5_1.gains[SPATIAL_CH_SR] = 0.55f;
    
    config->gains_7_1.gains[SPATIAL_CH_FL] = 1.0f;
    config->gains_7_1.gains[SPATIAL_CH_FR] = 1.0f;
    config->gains_7_1.gains[SPATIAL_CH_FC] = 0.760f;
    config->gains_7_1.gains[SPATIAL_CH_LFE] = 0.030f;
    config->gains_7_1.gains[SPATIAL_CH_SL] = 0.780f;
    config->gains_7_1.gains[SPATIAL_CH_SR] = 0.780f;
    config->gains_7_1.gains[SPATIAL_CH_BL] = 0.620f;
    config->gains_7_1.gains[SPATIAL_CH_BR] = 0.620f;
    
    config->matrix_oba = SPATIAL_MATRIX_NONE;
    config->matrix_cba = SPATIAL_MATRIX_NONE;
    config->content_type = SPATIAL_CONTENT_CBA;
    config->debug_enabled = false;
    config->generation = 0;
}

void spatial_config_copy(const spatial_config_t* src, spatial_config_t* dst) {
    if (!src || !dst) return;
    memcpy(dst, src, sizeof(spatial_config_t));
}

bool spatial_config_equal(const spatial_config_t* a, const spatial_config_t* b) {
    if (!a || !b) return false;
    return memcmp(a, b, sizeof(spatial_config_t)) == 0;
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
    if (read_prop_int(PROP_LAYOUT, &layout_val) && spatial_property_validate_layout(layout_val)) {
        out_config->layout = (spatial_layout_t)layout_val;
    }
    
    int debug_val;
    if (read_prop_int(PROP_DEBUG, &debug_val)) {
        out_config->debug_enabled = (debug_val != 0);
    }
    
    float gain_val;
    for (int i = 0; i < 5; i++) {
        if (read_prop_float(gain_5_1_names[i], &gain_val) && spatial_property_validate_gain(gain_val)) {
            out_config->gains_5_1.gains[i] = gain_val;
        }
    }
    
    for (int i = 0; i < 6; i++) {
        if (read_prop_float(gain_7_1_names[i], &gain_val) && spatial_property_validate_gain(gain_val)) {
            out_config->gains_7_1.gains[i] = gain_val;
        }
    }
    
    int matrix_val;
    if (read_prop_int(PROP_MATRIX_OBA, &matrix_val) && spatial_property_validate_matrix_mode(matrix_val)) {
        out_config->matrix_oba = (spatial_matrix_mode_t)matrix_val;
    }
    if (read_prop_int(PROP_MATRIX_CBA, &matrix_val) && spatial_property_validate_matrix_mode(matrix_val)) {
        out_config->matrix_cba = (spatial_matrix_mode_t)matrix_val;
    }
    
    char content_str[32];
    if (read_prop_string("persist.vendor.spatialdm.content_type", content_str, sizeof(content_str))) {
        if (strcmp(content_str, "oba") == 0) {
            out_config->content_type = SPATIAL_CONTENT_OBA;
        } else if (strcmp(content_str, "cba") == 0) {
            out_config->content_type = SPATIAL_CONTENT_CBA;
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
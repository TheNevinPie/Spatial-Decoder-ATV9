#ifndef SPATIAL_PROPERTY_BRIDGE_H
#define SPATIAL_PROPERTY_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPATIAL_LAYOUT_STEREO = 0,
    SPATIAL_LAYOUT_5_1    = 1,
    SPATIAL_LAYOUT_7_1    = 2,
} spatial_layout_t;

typedef enum {
    SPATIAL_MATRIX_NONE = 0,
    SPATIAL_MATRIX_DPLII = 1,
} spatial_matrix_mode_t;

typedef enum {
    SPATIAL_CONTENT_CBA = 0,
    SPATIAL_CONTENT_OBA = 1,
} spatial_content_type_t;

typedef enum {
    SPATIAL_CHANNEL_FL  = 0,
    SPATIAL_CHANNEL_FR  = 1,
    SPATIAL_CHANNEL_FC  = 2,
    SPATIAL_CHANNEL_LFE = 3,
    SPATIAL_CHANNEL_SL  = 4,
    SPATIAL_CHANNEL_SR  = 5,
    SPATIAL_CHANNEL_BL  = 6,
    SPATIAL_CHANNEL_BR  = 7,
    SPATIAL_CHANNEL_MAX = 8,
} spatial_channel_id_t;

typedef struct {
    float gains[SPATIAL_CHANNEL_MAX];
} spatial_gains_t;

typedef struct {
    spatial_layout_t layout;
    spatial_gains_t gains_5_1;
    spatial_gains_t gains_7_1;
    spatial_matrix_mode_t matrix_oba;
    spatial_matrix_mode_t matrix_cba;
    spatial_content_type_t content_type;
    bool debug_enabled;
    uint64_t generation;
} spatial_config_t;

typedef struct spatial_property_ctx spatial_property_ctx_t;

spatial_property_ctx_t* spatial_property_create(void);
void spatial_property_destroy(spatial_property_ctx_t* ctx);

int spatial_property_read_all(spatial_property_ctx_t* ctx, spatial_config_t* out_config);

int spatial_property_read_single(spatial_property_ctx_t* ctx, const char* key, float* out_value);
int spatial_property_read_enum(spatial_property_ctx_t* ctx, const char* key, int* out_value);

bool spatial_property_validate_gain(float value);
bool spatial_property_validate_layout(int value);
bool spatial_property_validate_matrix_mode(int value);
bool spatial_property_validate_content_type(int value);

void spatial_config_get_defaults(spatial_config_t* config);
void spatial_config_copy(const spatial_config_t* src, spatial_config_t* dst);
bool spatial_config_equal(const spatial_config_t* a, const spatial_config_t* b);

#ifdef __cplusplus
}
#endif

#endif
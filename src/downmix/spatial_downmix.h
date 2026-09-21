#ifndef SPATIAL_DOWNMIX_H
#define SPATIAL_DOWNMIX_H

#include <stdint.h>
#include <stddef.h>

#include "spatial_channel_layout.h"
#include "spatial_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float left;
    float right;
    float center;
    float surround;
    float lfe;
} spatial_gains_5_1_t;

typedef struct {
    float left;
    float right;
    float center;
    float side;
    float rear;
    float lfe;
} spatial_gains_7_1_t;

typedef struct {
    spatial_layout_t layout;
    int channel_map[SPATIAL_CH_MAX];

    spatial_gains_5_1_t gains_5_1;
    spatial_gains_7_1_t gains_7_1;

    spatial_matrix_mode_t matrix_oba;
    spatial_matrix_mode_t matrix_cba;

    spatial_content_type_t content_type;

    int debug_enabled;
} spatial_downmix_config_t;

typedef struct spatial_downmix_ctx spatial_downmix_ctx_t;
typedef struct spatial_config_mgr spatial_config_mgr_t;

spatial_downmix_ctx_t* spatial_downmix_create(const spatial_downmix_config_t* config);
void spatial_downmix_destroy(spatial_downmix_ctx_t* ctx);

int spatial_downmix_process(spatial_downmix_ctx_t* ctx,
                            const float* input,
                            float* output,
                            size_t frames);

int spatial_downmix_update_config(spatial_downmix_ctx_t* ctx, const spatial_downmix_config_t* config);

int spatial_downmix_set_config_manager(spatial_downmix_ctx_t* ctx, spatial_config_mgr_t* config_mgr);

int spatial_downmix_set_channel_map(spatial_downmix_ctx_t* ctx, const int* channel_map, int num_channels);

void spatial_downmix_get_default_config_5_1(spatial_downmix_config_t* config);
void spatial_downmix_get_default_config_7_1(spatial_downmix_config_t* config);

#ifdef __cplusplus
}
#endif

#endif
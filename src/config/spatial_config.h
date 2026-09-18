#ifndef SPATIAL_CONFIG_H
#define SPATIAL_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "spatial_channel_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPATIAL_MATRIX_NONE = 0,
    SPATIAL_MATRIX_DPLII = 1,
} spatial_matrix_mode_t;

typedef enum {
    SPATIAL_CONTENT_CBA = 0,
    SPATIAL_CONTENT_OBA = 1,
} spatial_content_type_t;

typedef spatial_channel_t spatial_channel_id_t;

typedef struct {
    float matrix[SPATIAL_CH_MAX][2];
    uint64_t generation;
    spatial_layout_t layout;
    int num_input_channels;
    bool valid;
} spatial_matrix_t;

typedef struct {
    spatial_layout_t layout;
    float gains_5_1[5];
    float gains_7_1[6];
    spatial_matrix_mode_t matrix_oba;
    spatial_matrix_mode_t matrix_cba;
    spatial_content_type_t content_type;
    bool debug_enabled;
    uint64_t generation;
} spatial_config_t;

typedef struct spatial_config_mgr spatial_config_mgr_t;

spatial_config_mgr_t* spatial_config_create(void);
void spatial_config_destroy(spatial_config_mgr_t* mgr);

void spatial_config_get_defaults(spatial_config_t* config);

int spatial_config_update(spatial_config_mgr_t* mgr, const spatial_config_t* new_config);
int spatial_config_update_from_property(spatial_config_mgr_t* mgr);

const spatial_matrix_t* spatial_config_get_matrix(spatial_config_mgr_t* mgr);
const spatial_config_t* spatial_config_get_current(spatial_config_mgr_t* mgr);

void spatial_config_build_matrix(const spatial_config_t* config, spatial_matrix_t* out_matrix);
bool spatial_config_matrix_valid(const spatial_matrix_t* matrix);

uint64_t spatial_config_get_generation(const spatial_config_t* config);

#ifdef __cplusplus
}
#endif

#endif
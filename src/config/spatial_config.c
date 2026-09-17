#include "spatial_config.h"
#include "property_bridge.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdbool.h>

#define MAX_CHANNELS 8

struct spatial_config_mgr {
    _Atomic(uintptr_t) current_ptr;
    spatial_config_t* configs[2];
    int active_index;
    _Atomic(uint64_t) generation_counter;
};

static const float C_TO_LR = 0.7071067811865475f;
static const float S_TO_LR = 0.7071067811865475f;
static const float REAR_TO_LR = 0.7071067811865475f;
static const float LFE_TO_LR = 1.0f;

static void build_matrix_5_1(const spatial_config_t* config, spatial_matrix_t* matrix) {
    memset(matrix, 0, sizeof(spatial_matrix_t));
    matrix->layout = SPATIAL_LAYOUT_5_1;
    matrix->num_input_channels = 6;
    matrix->generation = config->generation;
    
    matrix->matrix[SPATIAL_CHANNEL_FL][0] = config->gains_5_1[0];
    matrix->matrix[SPATIAL_CHANNEL_FR][1] = config->gains_5_1[1];
    matrix->matrix[SPATIAL_CHANNEL_FC][0] = config->gains_5_1[2] * C_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_FC][1] = config->gains_5_1[2] * C_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_LFE][0] = config->gains_5_1[4] * 1.0f;
    matrix->matrix[SPATIAL_CHANNEL_LFE][1] = config->gains_5_1[4] * 1.0f;
    matrix->matrix[SPATIAL_CHANNEL_SL][0] = config->gains_5_1[3] * S_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_SL][1] = config->gains_5_1[3] * S_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_SR][0] = config->gains_5_1[3] * S_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_SR][1] = config->gains_5_1[3] * S_TO_LR;
    
    matrix->valid = true;
}

static void build_matrix_7_1(const spatial_config_t* config, spatial_matrix_t* matrix) {
    memset(matrix, 0, sizeof(spatial_matrix_t));
    matrix->layout = SPATIAL_LAYOUT_7_1;
    matrix->num_input_channels = 8;
    matrix->generation = config->generation;
    
    matrix->matrix[SPATIAL_CHANNEL_FL][0] = config->gains_7_1[0];
    matrix->matrix[SPATIAL_CHANNEL_FR][1] = config->gains_7_1[1];
    matrix->matrix[SPATIAL_CHANNEL_FC][0] = config->gains_7_1[2] * C_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_FC][1] = config->gains_7_1[2] * C_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_LFE][0] = config->gains_7_1[5] * 1.0f;
    matrix->matrix[SPATIAL_CHANNEL_LFE][1] = config->gains_7_1[5] * 1.0f;
    matrix->matrix[SPATIAL_CHANNEL_SL][0] = config->gains_7_1[3] * S_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_SL][1] = config->gains_7_1[3] * S_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_SR][0] = config->gains_7_1[3] * S_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_SR][1] = config->gains_7_1[3] * S_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_BL][0] = config->gains_7_1[4] * REAR_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_BL][1] = config->gains_7_1[4] * REAR_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_BR][0] = config->gains_7_1[4] * REAR_TO_LR;
    matrix->matrix[SPATIAL_CHANNEL_BR][1] = config->gains_7_1[4] * REAR_TO_LR;
    
    matrix->valid = true;
}

static void build_matrix(const spatial_config_t* config, spatial_matrix_t* matrix) {
    switch (config->layout) {
        case SPATIAL_LAYOUT_5_1:
            build_matrix_5_1(config, matrix);
            break;
        case SPATIAL_LAYOUT_7_1:
            build_matrix_7_1(config, matrix);
            break;
        case SPATIAL_LAYOUT_STEREO:
        default:
            memset(matrix, 0, sizeof(spatial_matrix_t));
            matrix->layout = SPATIAL_LAYOUT_STEREO;
            matrix->num_input_channels = 2;
            matrix->generation = config->generation;
            matrix->matrix[0][0] = 1.0f;
            matrix->matrix[1][1] = 1.0f;
            matrix->valid = true;
            break;
    }
}

void spatial_config_get_defaults(spatial_config_t* config) {
    if (!config) return;
    memset(config, 0, sizeof(spatial_config_t));
    config->layout = SPATIAL_LAYOUT_5_1;
    config->gains_5_1[0] = 1.0f;
    config->gains_5_1[1] = 1.0f;
    config->gains_5_1[2] = 0.90f;
    config->gains_5_1[3] = 0.55f;
    config->gains_5_1[4] = 0.25f;
    config->gains_7_1[0] = 1.0f;
    config->gains_7_1[1] = 1.0f;
    config->gains_7_1[2] = 0.760f;
    config->gains_7_1[3] = 0.780f;
    config->gains_7_1[4] = 0.620f;
    config->gains_7_1[5] = 0.030f;
    config->matrix_oba = 0;
    config->matrix_cba = 0;
    config->content_type = 0;
    config->debug_enabled = false;
    config->generation = 0;
}

spatial_config_mgr_t* spatial_config_create(void) {
    spatial_config_mgr_t* mgr = calloc(1, sizeof(spatial_config_mgr_t));
    if (!mgr) return NULL;
    
    mgr->configs[0] = calloc(1, sizeof(spatial_config_t));
    mgr->configs[1] = calloc(1, sizeof(spatial_config_t));
    if (!mgr->configs[0] || !mgr->configs[1]) {
        free(mgr->configs[0]);
        free(mgr->configs[1]);
        free(mgr);
        return NULL;
    }
    
    spatial_config_get_defaults(mgr->configs[0]);
    spatial_config_get_defaults(mgr->configs[1]);
    
    mgr->active_index = 0;
    atomic_init(&mgr->current_ptr, (uintptr_t)mgr->configs[0]);
    atomic_init(&mgr->generation_counter, 1);
    
    return mgr;
}

void spatial_config_destroy(spatial_config_mgr_t* mgr) {
    if (!mgr) return;
    free(mgr->configs[0]);
    free(mgr->configs[1]);
    free(mgr);
}

int spatial_config_update(spatial_config_mgr_t* mgr, const spatial_config_t* new_config) {
    if (!mgr || !new_config) return -1;
    
    int next_index = 1 - mgr->active_index;
    spatial_config_t* next_config = mgr->configs[next_index];
    
    memcpy(next_config, new_config, sizeof(spatial_config_t));
    next_config->generation = atomic_fetch_add(&mgr->generation_counter, 1) + 1;
    
    if (new_config->debug_enabled) {
        printf("[spatial_config] Update: gen=%lu layout=%d\n", 
               (unsigned long)next_config->generation, new_config->layout);
    }
    
    atomic_store(&mgr->current_ptr, (uintptr_t)next_config);
    mgr->active_index = next_index;
    
    return 0;
}

int spatial_config_update_from_property(spatial_config_mgr_t* mgr) {
    if (!mgr) return -1;
    
    spatial_property_ctx_t* prop_ctx = spatial_property_create();
    if (!prop_ctx) return -1;
    
    spatial_config_t new_config;
    int ret = spatial_property_read_all(prop_ctx, &new_config);
    spatial_property_destroy(prop_ctx);
    
    if (ret != 0) return ret;
    
    return spatial_config_update(mgr, &new_config);
}

const spatial_matrix_t* spatial_config_get_matrix(spatial_config_mgr_t* mgr) {
    static _Thread_local spatial_matrix_t thread_local_matrix;
    const spatial_config_t* config = (const spatial_config_t*)atomic_load(&mgr->current_ptr);
    build_matrix(config, &thread_local_matrix);
    return &thread_local_matrix;
}

const spatial_config_t* spatial_config_get_current(spatial_config_mgr_t* mgr) {
    return (const spatial_config_t*)atomic_load(&mgr->current_ptr);
}

void spatial_config_build_matrix(const spatial_config_t* config, spatial_matrix_t* out_matrix) {
    if (!config || !out_matrix) return;
    build_matrix(config, out_matrix);
}

bool spatial_config_matrix_valid(const spatial_matrix_t* matrix) {
    return matrix && matrix->valid;
}

uint64_t spatial_config_get_generation(const spatial_config_t* config) {
    return config ? config->generation : 0;
}
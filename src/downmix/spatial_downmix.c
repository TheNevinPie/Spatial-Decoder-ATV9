#include "spatial_downmix.h"
#include "spatial_config.h"
#include "spatial_channel_layout.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define MAX_CHANNELS 8

struct spatial_downmix_ctx {
    spatial_downmix_config_t config;
    spatial_config_mgr_t* config_mgr;
    float matrix[MAX_CHANNELS][2];
    int matrix_valid;
    spatial_layout_t current_layout;
    uint64_t last_matrix_generation;
    int channel_map[MAX_CHANNELS];
    int num_input_channels;
};

static const float C_TO_LR = 0.7071067811865475f;
static const float S_TO_LR = 0.7071067811865475f;
static const float REAR_TO_LR = 0.7071067811865475f;
static const float LFE_TO_LR = 1.0f;

static void init_default_channel_map(spatial_downmix_ctx_t* ctx) {
    switch (ctx->config.layout) {
        case SPATIAL_LAYOUT_5_1: {
            ctx->channel_map[0] = SPATIAL_CHANNEL_FL;
            ctx->channel_map[1] = SPATIAL_CHANNEL_FR;
            ctx->channel_map[2] = SPATIAL_CHANNEL_FC;
            ctx->channel_map[3] = SPATIAL_CHANNEL_LFE;
            ctx->channel_map[4] = SPATIAL_CHANNEL_SL;
            ctx->channel_map[5] = SPATIAL_CHANNEL_SR;
            ctx->num_input_channels = 6;
            break;
        }
        case SPATIAL_LAYOUT_7_1: {
            ctx->channel_map[0] = SPATIAL_CHANNEL_FL;
            ctx->channel_map[1] = SPATIAL_CHANNEL_FR;
            ctx->channel_map[2] = SPATIAL_CHANNEL_FC;
            ctx->channel_map[3] = SPATIAL_CHANNEL_LFE;
            ctx->channel_map[4] = SPATIAL_CHANNEL_SL;
            ctx->channel_map[5] = SPATIAL_CHANNEL_SR;
            ctx->channel_map[6] = SPATIAL_CHANNEL_BL;
            ctx->channel_map[7] = SPATIAL_CHANNEL_BR;
            ctx->num_input_channels = 8;
            break;
        }
        case SPATIAL_LAYOUT_STEREO:
        default: {
            ctx->channel_map[0] = SPATIAL_CHANNEL_FL;
            ctx->channel_map[1] = SPATIAL_CHANNEL_FR;
            ctx->num_input_channels = 2;
            break;
        }
    }
}

static void build_matrix_for_layout(const spatial_downmix_config_t* config, spatial_layout_t layout, float matrix[MAX_CHANNELS][2]) {
    memset(matrix, 0, sizeof(float) * MAX_CHANNELS * 2);
    
    switch (layout) {
        case SPATIAL_LAYOUT_5_1: {
            matrix[SPATIAL_CHANNEL_FL][0] = config->gains_5_1.left;
            matrix[SPATIAL_CHANNEL_FR][1] = config->gains_5_1.right;
            matrix[SPATIAL_CHANNEL_FC][0] = config->gains_5_1.center * C_TO_LR;
            matrix[SPATIAL_CHANNEL_FC][1] = config->gains_5_1.center * C_TO_LR;
            matrix[SPATIAL_CHANNEL_LFE][0] = config->gains_5_1.lfe * LFE_TO_LR;
            matrix[SPATIAL_CHANNEL_LFE][1] = config->gains_5_1.lfe * LFE_TO_LR;
            matrix[SPATIAL_CHANNEL_SL][0] = config->gains_5_1.surround * S_TO_LR;
            matrix[SPATIAL_CHANNEL_SL][1] = config->gains_5_1.surround * S_TO_LR;
            matrix[SPATIAL_CHANNEL_SR][0] = config->gains_5_1.surround * S_TO_LR;
            matrix[SPATIAL_CHANNEL_SR][1] = config->gains_5_1.surround * S_TO_LR;
            break;
        }
        case SPATIAL_LAYOUT_7_1: {
            matrix[SPATIAL_CHANNEL_FL][0] = config->gains_7_1.left;
            matrix[SPATIAL_CHANNEL_FR][1] = config->gains_7_1.right;
            matrix[SPATIAL_CHANNEL_FC][0] = config->gains_7_1.center * C_TO_LR;
            matrix[SPATIAL_CHANNEL_FC][1] = config->gains_7_1.center * C_TO_LR;
            matrix[SPATIAL_CHANNEL_LFE][0] = config->gains_7_1.lfe * LFE_TO_LR;
            matrix[SPATIAL_CHANNEL_LFE][1] = config->gains_7_1.lfe * LFE_TO_LR;
            matrix[SPATIAL_CHANNEL_SL][0] = config->gains_7_1.side * S_TO_LR;
            matrix[SPATIAL_CHANNEL_SL][1] = config->gains_7_1.side * S_TO_LR;
            matrix[SPATIAL_CHANNEL_SR][0] = config->gains_7_1.side * S_TO_LR;
            matrix[SPATIAL_CHANNEL_SR][1] = config->gains_7_1.side * S_TO_LR;
            matrix[SPATIAL_CHANNEL_BL][0] = config->gains_7_1.rear * REAR_TO_LR;
            matrix[SPATIAL_CHANNEL_BL][1] = config->gains_7_1.rear * REAR_TO_LR;
            matrix[SPATIAL_CHANNEL_BR][0] = config->gains_7_1.rear * REAR_TO_LR;
            matrix[SPATIAL_CHANNEL_BR][1] = config->gains_7_1.rear * REAR_TO_LR;
            break;
        }
        case SPATIAL_LAYOUT_STEREO:
        default: {
            matrix[SPATIAL_CHANNEL_FL][0] = 1.0f;
            matrix[SPATIAL_CHANNEL_FR][1] = 1.0f;
            break;
        }
    }
}

static void rebuild_matrix(spatial_downmix_ctx_t* ctx) {
    build_matrix_for_layout(&ctx->config, ctx->config.layout, ctx->matrix);
    ctx->current_layout = ctx->config.layout;
    ctx->matrix_valid = 1;
}

static void ensure_matrix_up_to_date(spatial_downmix_ctx_t* ctx) {
    if (ctx->config_mgr) {
        const spatial_matrix_t* matrix = spatial_config_get_matrix(ctx->config_mgr);
        if (matrix && matrix->valid && matrix->generation != ctx->last_matrix_generation) {
            for (int i = 0; i < MAX_CHANNELS; i++) {
                ctx->matrix[i][0] = matrix->matrix[i][0];
                ctx->matrix[i][1] = matrix->matrix[i][1];
            }
            ctx->current_layout = matrix->layout;
            ctx->last_matrix_generation = matrix->generation;
            ctx->matrix_valid = 1;
        }
    } else {
        if (!ctx->matrix_valid || ctx->current_layout != ctx->config.layout) {
            rebuild_matrix(ctx);
        }
    }
}

static void rebuild_channel_map(spatial_downmix_ctx_t* ctx) {
    init_default_channel_map(ctx);
}

spatial_downmix_ctx_t* spatial_downmix_create(const spatial_downmix_config_t* config) {
    spatial_downmix_ctx_t* ctx = calloc(1, sizeof(spatial_downmix_ctx_t));
    if (!ctx) return NULL;

    if (config) {
        ctx->config = *config;
    } else {
        spatial_downmix_get_default_config_5_1(&ctx->config);
    }

    ctx->matrix_valid = 0;
    ctx->current_layout = SPATIAL_LAYOUT_STEREO;
    ctx->last_matrix_generation = 0;
    ctx->config_mgr = NULL;
    
    init_default_channel_map(ctx);

    if (ctx->config.debug_enabled) {
        printf("[spatial_downmix] Created: layout=%d\n", ctx->config.layout);
    }

    return ctx;
}

void spatial_downmix_destroy(spatial_downmix_ctx_t* ctx) {
    if (ctx) {
        free(ctx);
    }
}

int spatial_downmix_set_config_manager(spatial_downmix_ctx_t* ctx, spatial_config_mgr_t* config_mgr) {
    if (!ctx) return -1;
    ctx->config_mgr = config_mgr;
    ctx->matrix_valid = 0;
    return 0;
}

int spatial_downmix_set_channel_map(spatial_downmix_ctx_t* ctx, const int* channel_map, int num_channels) {
    if (!ctx || !channel_map || num_channels <= 0 || num_channels > MAX_CHANNELS) return -1;
    
    for (int i = 0; i < num_channels; i++) {
        ctx->channel_map[i] = channel_map[i];
    }
    ctx->num_input_channels = num_channels;
    ctx->matrix_valid = 0; // Rebuild matrix with new mapping
    
    if (ctx->config.debug_enabled) {
        printf("[spatial_downmix] Channel map updated: %d channels\n", num_channels);
    }
    return 0;
}

static int gains_equal_5_1(const spatial_gains_5_1_t* a, const spatial_gains_5_1_t* b) {
    return a->left == b->left &&
           a->right == b->right &&
           a->center == b->center &&
           a->surround == b->surround &&
           a->lfe == b->lfe;
}

static int gains_equal_7_1(const spatial_gains_7_1_t* a, const spatial_gains_7_1_t* b) {
    return a->left == b->left &&
           a->right == b->right &&
           a->center == b->center &&
           a->side == b->side &&
           a->rear == b->rear &&
           a->lfe == b->lfe;
}

int spatial_downmix_update_config(spatial_downmix_ctx_t* ctx, const spatial_downmix_config_t* config) {
    if (!ctx || !config) return -1;

    int layout_changed = (ctx->config.layout != config->layout);
    int gains_changed = 0;
    
    // Check 5.1 gains
    if (config->layout == SPATIAL_LAYOUT_5_1) {
        if (!gains_equal_5_1(&ctx->config.gains_5_1, &config->gains_5_1)) {
            gains_changed = 1;
        }
    }
    
    // Check 7.1 gains
    if (config->layout == SPATIAL_LAYOUT_7_1) {
        if (!gains_equal_7_1(&ctx->config.gains_7_1, &config->gains_7_1)) {
            gains_changed = 1;
        }
    }

    ctx->config = *config;

    if (layout_changed) {
        init_default_channel_map(ctx);
    }

    if (layout_changed || gains_changed) {
        ctx->matrix_valid = 0;
    }

    if (ctx->config.debug_enabled) {
        printf("[spatial_downmix] Config updated: layout=%d\n", ctx->config.layout);
    }

    return 0;
}

int spatial_downmix_process(spatial_downmix_ctx_t* ctx,
                            const float* input,
                            float* output,
                            size_t frames) {
    if (!ctx || !input || !output) return -1;

    ensure_matrix_up_to_date(ctx);

    int num_input_channels = ctx->num_input_channels;
    if (num_input_channels <= 0) {
        switch (ctx->current_layout) {
            case SPATIAL_LAYOUT_5_1: num_input_channels = 6; break;
            case SPATIAL_LAYOUT_7_1: num_input_channels = 8; break;
            case SPATIAL_LAYOUT_STEREO:
            default: num_input_channels = 2; break;
        }
    }

    for (size_t f = 0; f < frames; f++) {
        float l = 0.0f;
        float r = 0.0f;

        for (int c = 0; c < num_input_channels; c++) {
            int logical_ch = ctx->channel_map[c];
            if (logical_ch >= 0 && logical_ch < MAX_CHANNELS) {
                l += input[f * num_input_channels + c] * ctx->matrix[logical_ch][0];
                r += input[f * num_input_channels + c] * ctx->matrix[logical_ch][1];
            }
        }

        output[f * 2 + 0] = l;
        output[f * 2 + 1] = r;
    }

    return 0;
}

void spatial_downmix_get_default_config_5_1(spatial_downmix_config_t* config) {
    if (!config) return;

    memset(config, 0, sizeof(spatial_downmix_config_t));
    config->layout = SPATIAL_LAYOUT_5_1;

    config->gains_5_1.left = 1.0f;
    config->gains_5_1.right = 1.0f;
    config->gains_5_1.center = 0.90f;
    config->gains_5_1.surround = 0.55f;
    config->gains_5_1.lfe = 0.25f;

    config->gains_7_1.left = 1.0f;
    config->gains_7_1.right = 1.0f;
    config->gains_7_1.center = 0.760f;
    config->gains_7_1.side = 0.780f;
    config->gains_7_1.rear = 0.620f;
    config->gains_7_1.lfe = 0.030f;

    config->matrix_oba = SPATIAL_MATRIX_NONE;
    config->matrix_cba = SPATIAL_MATRIX_NONE;
    config->content_type = SPATIAL_CONTENT_CBA;
    config->debug_enabled = 0;
}

void spatial_downmix_get_default_config_7_1(spatial_downmix_config_t* config) {
    if (!config) return;

    spatial_downmix_get_default_config_5_1(config);
    config->layout = SPATIAL_LAYOUT_7_1;
}
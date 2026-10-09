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
    // Linked-stereo DRC envelope: single linear gain applied to L and R
    // together (never independent per channel: that would move the stereo
    // image). Unity when DRC is off or after reset.
    float drc_env;
};

static const float C_TO_LR = 0.7071067811865475f;
static const float S_TO_LR = 0.7071067811865475f;
static const float REAR_TO_LR = 0.7071067811865475f;
static const float LFE_TO_LR = 1.0f;

// Fixed DRC profiles (tunable starting points). The downmix output is
// always 48 kHz stereo in this system; ballistics below assume that rate.
#define SPATIAL_DRC_RATE 48000.0f

typedef struct {
    float threshold_db;  // static curve threshold, dBFS
    float ratio;         // compression ratio above threshold
    float attack_ms;     // envelope attack time constant
    float release_ms;    // envelope release time constant
    float makeup_db;     // output makeup (0 keeps peaks reduced, never boosts)
    float knee_db;       // soft-knee width around threshold
} drc_profile_t;

static const drc_profile_t kDrcProfiles[3] = {
    // OFF (unused; process() bypasses DRC entirely in this mode).
    {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    // FILM: gentle leveling above -18 dBFS.
    {-18.0f, 2.0f, 10.0f, 150.0f, 0.0f, 6.0f},
    // NIGHT: stronger leveling above -30 dBFS, no blind makeup gain.
    {-30.0f, 4.0f, 5.0f, 250.0f, 0.0f, 6.0f},
};

// Static soft-knee gain computer. Returns the LINEAR target gain for an
// instantaneous linked peak (0..1 output; never boosts: the makeup field
// is reserved for measured tuning and clamped here).
static float drc_target_gain(const drc_profile_t* p, float peak) {
    // peak <= 0 covers silence; !(peak > 0) additionally covers NaN.
    if (!(peak > 0.0f)) {
        return 1.0f;
    }
    float indb = 20.0f * log10f(peak);
    float outdb;
    float half_knee = p->knee_db * 0.5f;
    if (indb <= p->threshold_db - half_knee) {
        outdb = indb;
    } else if (indb >= p->threshold_db + half_knee) {
        outdb = p->threshold_db + (indb - p->threshold_db) / p->ratio;
    } else {
        float x = indb - p->threshold_db + half_knee;
        outdb = indb + (1.0f / p->ratio - 1.0f)
                * x * x / (2.0f * p->knee_db);
    }
    outdb += p->makeup_db;
    float grdb = outdb - indb;
    if (grdb >= 0.0f) {
        return 1.0f;
    }
    return powf(10.0f, grdb * (1.0f / 20.0f));
}

void spatial_downmix_reset(spatial_downmix_ctx_t* ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->drc_env = 1.0f;
}

static void init_default_channel_map(spatial_downmix_ctx_t* ctx) {
    switch (ctx->config.layout) {
        case SPATIAL_LAYOUT_5_1: {
            ctx->channel_map[0] = SPATIAL_CH_FL;
            ctx->channel_map[1] = SPATIAL_CH_FR;
            ctx->channel_map[2] = SPATIAL_CH_FC;
            ctx->channel_map[3] = SPATIAL_CH_LFE;
            ctx->channel_map[4] = SPATIAL_CH_SL;
            ctx->channel_map[5] = SPATIAL_CH_SR;
            ctx->num_input_channels = 6;
            break;
        }
        case SPATIAL_LAYOUT_7_1: {
            ctx->channel_map[0] = SPATIAL_CH_FL;
            ctx->channel_map[1] = SPATIAL_CH_FR;
            ctx->channel_map[2] = SPATIAL_CH_FC;
            ctx->channel_map[3] = SPATIAL_CH_LFE;
            ctx->channel_map[4] = SPATIAL_CH_SL;
            ctx->channel_map[5] = SPATIAL_CH_SR;
            ctx->channel_map[6] = SPATIAL_CH_BL;
            ctx->channel_map[7] = SPATIAL_CH_BR;
            ctx->num_input_channels = 8;
            break;
        }
        case SPATIAL_LAYOUT_STEREO:
        default: {
            ctx->channel_map[0] = SPATIAL_CH_FL;
            ctx->channel_map[1] = SPATIAL_CH_FR;
            ctx->num_input_channels = 2;
            break;
        }
    }
}

static void build_matrix_for_layout(const spatial_downmix_config_t* config, spatial_layout_t layout, float matrix[MAX_CHANNELS][2]) {
    memset(matrix, 0, sizeof(float) * MAX_CHANNELS * 2);
    
    switch (layout) {
        case SPATIAL_LAYOUT_5_1: {
            matrix[SPATIAL_CH_FL][0] = config->gains_5_1.left;
            matrix[SPATIAL_CH_FR][1] = config->gains_5_1.right;
            matrix[SPATIAL_CH_FC][0] = config->gains_5_1.center * C_TO_LR;
            matrix[SPATIAL_CH_FC][1] = config->gains_5_1.center * C_TO_LR;
            matrix[SPATIAL_CH_LFE][0] = config->gains_5_1.lfe * LFE_TO_LR;
            matrix[SPATIAL_CH_LFE][1] = config->gains_5_1.lfe * LFE_TO_LR;
            matrix[SPATIAL_CH_SL][0] = config->gains_5_1.surround_left * S_TO_LR;
            matrix[SPATIAL_CH_SL][1] = config->gains_5_1.surround_left * S_TO_LR;
            matrix[SPATIAL_CH_SR][0] = config->gains_5_1.surround_right * S_TO_LR;
            matrix[SPATIAL_CH_SR][1] = config->gains_5_1.surround_right * S_TO_LR;
            break;
        }
        case SPATIAL_LAYOUT_7_1: {
            matrix[SPATIAL_CH_FL][0] = config->gains_7_1.left;
            matrix[SPATIAL_CH_FR][1] = config->gains_7_1.right;
            matrix[SPATIAL_CH_FC][0] = config->gains_7_1.center * C_TO_LR;
            matrix[SPATIAL_CH_FC][1] = config->gains_7_1.center * C_TO_LR;
            matrix[SPATIAL_CH_LFE][0] = config->gains_7_1.lfe * LFE_TO_LR;
            matrix[SPATIAL_CH_LFE][1] = config->gains_7_1.lfe * LFE_TO_LR;
            matrix[SPATIAL_CH_SL][0] = config->gains_7_1.side_left * S_TO_LR;
            matrix[SPATIAL_CH_SL][1] = config->gains_7_1.side_left * S_TO_LR;
            matrix[SPATIAL_CH_SR][0] = config->gains_7_1.side_right * S_TO_LR;
            matrix[SPATIAL_CH_SR][1] = config->gains_7_1.side_right * S_TO_LR;
            matrix[SPATIAL_CH_BL][0] = config->gains_7_1.rear_left * REAR_TO_LR;
            matrix[SPATIAL_CH_BL][1] = config->gains_7_1.rear_left * REAR_TO_LR;
            matrix[SPATIAL_CH_BR][0] = config->gains_7_1.rear_right * REAR_TO_LR;
            matrix[SPATIAL_CH_BR][1] = config->gains_7_1.rear_right * REAR_TO_LR;
            break;
        }
        case SPATIAL_LAYOUT_STEREO:
        default: {
            matrix[SPATIAL_CH_FL][0] = 1.0f;
            matrix[SPATIAL_CH_FR][1] = 1.0f;
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
            // DRC mode follows the same generation so the standalone
            // manager path and the direct update_config path agree.
            {
                const spatial_config_t* cur =
                        spatial_config_get_current(ctx->config_mgr);
                if (cur != NULL && cur->drc_mode >= SPATIAL_DRC_OFF
                        && cur->drc_mode <= SPATIAL_DRC_NIGHT) {
                    ctx->config.drc_mode = cur->drc_mode;
                }
            }
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
    ctx->drc_env = 1.0f;
    
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
           a->surround_left == b->surround_left &&
           a->surround_right == b->surround_right &&
           a->lfe == b->lfe;
}

static int gains_equal_7_1(const spatial_gains_7_1_t* a, const spatial_gains_7_1_t* b) {
    return a->left == b->left &&
           a->right == b->right &&
           a->center == b->center &&
           a->side_left == b->side_left &&
           a->side_right == b->side_right &&
           a->rear_left == b->rear_left &&
           a->rear_right == b->rear_right &&
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

    // OFF path: pure linear downmix, exactly the pre-DRC behavior.
    if (ctx->config.drc_mode != SPATIAL_DRC_FILM
            && ctx->config.drc_mode != SPATIAL_DRC_NIGHT) {
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

    // Linked-stereo DRC path: identical matrix mix, then one shared
    // gain-reduction envelope applied to L and R together (the detector
    // level comes from both channels, so the image never shifts).
    // Ballistics resolved once per call; no allocation, no locks,
    // no property access. Envelope state lives in the ctx (preallocated
    // at create, cleared by spatial_downmix_reset on flush).
    {
        const drc_profile_t* prof =
                &kDrcProfiles[(int)ctx->config.drc_mode];
        float attack_c = expf(-1.0f
                / (SPATIAL_DRC_RATE * (prof->attack_ms / 1000.0f)));
        float release_c = expf(-1.0f
                / (SPATIAL_DRC_RATE * (prof->release_ms / 1000.0f)));
        float env = ctx->drc_env;
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

            float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            float target = drc_target_gain(prof, peak);
            float c = (target < env) ? attack_c : release_c;
            env = target + (env - target) * c;
            output[f * 2 + 0] = l * env;
            output[f * 2 + 1] = r * env;
        }
        ctx->drc_env = env;
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
    config->gains_5_1.surround_left = 0.55f;
    config->gains_5_1.surround_right = 0.55f;
    config->gains_5_1.lfe = 0.25f;

    config->gains_7_1.left = 1.0f;
    config->gains_7_1.right = 1.0f;
    config->gains_7_1.center = 0.760f;
    config->gains_7_1.side_left = 0.780f;
    config->gains_7_1.side_right = 0.780f;
    config->gains_7_1.rear_left = 0.620f;
    config->gains_7_1.rear_right = 0.620f;
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
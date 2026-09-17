#include "spatial_downmix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SAMPLE_RATE 48000
#define DURATION_SEC 1.0
#define NUM_FRAMES (SAMPLE_RATE * DURATION_SEC)

typedef struct {
    float rms_left;
    float rms_right;
    float peak_left;
    float peak_right;
    int clipping_left;
    int clipping_right;
} analysis_t;

static void analyze_stereo(const float* data, size_t frames, analysis_t* out) {
    float sum_l = 0, sum_r = 0;
    float peak_l = 0, peak_r = 0;
    int clip_l = 0, clip_r = 0;

    for (size_t i = 0; i < frames; i++) {
        float l = data[i * 2];
        float r = data[i * 2 + 1];
        sum_l += l * l;
        sum_r += r * r;
        if (fabsf(l) > peak_l) peak_l = fabsf(l);
        if (fabsf(r) > peak_r) peak_r = fabsf(r);
        if (fabsf(l) >= 1.0f) clip_l++;
        if (fabsf(r) >= 1.0f) clip_r++;
    }

    out->rms_left = sqrtf(sum_l / frames);
    out->rms_right = sqrtf(sum_r / frames);
    out->peak_left = peak_l;
    out->peak_right = peak_r;
    out->clipping_left = clip_l;
    out->clipping_right = clip_r;
}

static void print_analysis(const char* label, const analysis_t* a) {
    printf("%s:\n", label);
    printf("  RMS:   L=%.6f  R=%.6f\n", a->rms_left, a->rms_right);
    printf("  Peak:  L=%.6f  R=%.6f\n", a->peak_left, a->peak_right);
    printf("  Clip:  L=%d  R=%d\n", a->clipping_left, a->clipping_right);
}

static void generate_tone(float* buf, size_t frames, int channel, float freq, float amplitude) {
    for (size_t i = 0; i < frames; i++) {
        float t = (float)i / SAMPLE_RATE;
        float sample = amplitude * sinf(2.0f * M_PI * freq * t);
        for (int c = 0; c < 6; c++) {
            buf[i * 6 + c] = 0.0f;
        }
        buf[i * 6 + channel] = sample;
    }
}

static void test_isolated_channel_5_1(int channel, const char* name, float gain) {
    printf("\n=== Test: Isolated %s (gain=%.2f) ===\n", name, gain);

    float* input = malloc(NUM_FRAMES * 6 * sizeof(float));
    float* output = malloc(NUM_FRAMES * 2 * sizeof(float));

    generate_tone(input, NUM_FRAMES, channel, 440.0f, 1.0f);

    spatial_downmix_config_t config;
    spatial_downmix_get_default_config_5_1(&config);
    config.debug_enabled = 1;

    switch (channel) {
        case 0: config.gains_5_1.left = gain; break;
        case 1: config.gains_5_1.right = gain; break;
        case 2: config.gains_5_1.center = gain; break;
        case 3: config.gains_5_1.lfe = gain; break;
        case 4: config.gains_5_1.surround = gain; break;
        case 5: config.gains_5_1.surround = gain; break;
    }

    spatial_downmix_ctx_t* ctx = spatial_downmix_create(&config);
    spatial_downmix_process(ctx, input, output, NUM_FRAMES);
    spatial_downmix_destroy(ctx);

    analysis_t a;
    analyze_stereo(output, NUM_FRAMES, &a);
    print_analysis("Output", &a);

    free(input);
    free(output);
}

static void test_5_1_defaults() {
    printf("\n=== Test: 5.1 Default Configuration ===\n");

    float* input = malloc(NUM_FRAMES * 6 * sizeof(float));
    float* output = malloc(NUM_FRAMES * 2 * sizeof(float));

    for (size_t i = 0; i < NUM_FRAMES; i++) {
        for (int c = 0; c < 6; c++) {
            input[i * 6 + c] = 0.0f;
        }
    }

    spatial_downmix_config_t config;
    spatial_downmix_get_default_config_5_1(&config);
    config.debug_enabled = 1;

    spatial_downmix_ctx_t* ctx = spatial_downmix_create(&config);
    spatial_downmix_process(ctx, input, output, NUM_FRAMES);
    spatial_downmix_destroy(ctx);

    analysis_t a;
    analyze_stereo(output, NUM_FRAMES, &a);
    print_analysis("Silence Output", &a);

    free(input);
    free(output);
}

static void test_stereo_passthrough() {
    printf("\n=== Test: Stereo Passthrough ===\n");

    float* input = malloc(NUM_FRAMES * 2 * sizeof(float));
    float* output = malloc(NUM_FRAMES * 2 * sizeof(float));

    for (size_t i = 0; i < NUM_FRAMES; i++) {
        float t = (float)i / SAMPLE_RATE;
        input[i * 2 + 0] = 0.5f * sinf(2.0f * M_PI * 440.0f * t);
        input[i * 2 + 1] = 0.5f * sinf(2.0f * M_PI * 550.0f * t);
    }

    spatial_downmix_config_t config;
    spatial_downmix_get_default_config_5_1(&config);
    config.layout = SPATIAL_LAYOUT_STEREO;

    spatial_downmix_ctx_t* ctx = spatial_downmix_create(&config);
    spatial_downmix_process(ctx, input, output, NUM_FRAMES);
    spatial_downmix_destroy(ctx);

    analysis_t a;
    analyze_stereo(output, NUM_FRAMES, &a);
    print_analysis("Stereo Output", &a);

    free(input);
    free(output);
}

static void test_live_config_update() {
    printf("\n=== Test: Live Config Update ===\n");

    float* input = malloc(NUM_FRAMES * 6 * sizeof(float));
    float* output = malloc(NUM_FRAMES * 2 * sizeof(float));

    generate_tone(input, NUM_FRAMES, 2, 440.0f, 1.0f);

    spatial_downmix_config_t config;
    spatial_downmix_get_default_config_5_1(&config);

    spatial_downmix_ctx_t* ctx = spatial_downmix_create(&config);

    config.gains_5_1.center = 0.7f;
    spatial_downmix_update_config(ctx, &config);
    spatial_downmix_process(ctx, input, output, NUM_FRAMES);

    analysis_t a1;
    analyze_stereo(output, NUM_FRAMES, &a1);
    print_analysis("center=0.7", &a1);

    config.gains_5_1.center = 1.0f;
    spatial_downmix_update_config(ctx, &config);
    spatial_downmix_process(ctx, input, output, NUM_FRAMES);

    analysis_t a2;
    analyze_stereo(output, NUM_FRAMES, &a2);
    print_analysis("center=1.0", &a2);

    config.gains_5_1.center = 1.3f;
    spatial_downmix_update_config(ctx, &config);
    spatial_downmix_process(ctx, input, output, NUM_FRAMES);

    analysis_t a3;
    analyze_stereo(output, NUM_FRAMES, &a3);
    print_analysis("center=1.3", &a3);

    float ratio_1_0 = a2.rms_left / a1.rms_left;
    float expected = 1.0f / 0.7f;
    printf("Gain ratio (1.0/0.7): measured=%.4f expected=%.4f diff=%.4f\n",
           ratio_1_0, expected, fabsf(ratio_1_0 - expected));

    spatial_downmix_destroy(ctx);
    free(input);
    free(output);
}

int main() {
    printf("========================================\n");
    printf("Spatial Downmix Engine - Standalone Test\n");
    printf("========================================\n");

    test_5_1_defaults();
    test_stereo_passthrough();

    test_isolated_channel_5_1(0, "FL (Front Left)", 1.0f);
    test_isolated_channel_5_1(1, "FR (Front Right)", 1.0f);
    test_isolated_channel_5_1(2, "C  (Center)", 1.0f);
    test_isolated_channel_5_1(3, "LFE", 1.0f);
    test_isolated_channel_5_1(4, "SL (Side Left)", 1.0f);
    test_isolated_channel_5_1(5, "SR (Side Right)", 1.0f);

    test_live_config_update();

    printf("\n=== All Tests Complete ===\n");
    return 0;
}
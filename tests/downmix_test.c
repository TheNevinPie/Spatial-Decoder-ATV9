#include "spatial_downmix.h"
#include "property_bridge.h"
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
        case 4: config.gains_5_1.surround_left = gain; break;
        case 5: config.gains_5_1.surround_right = gain; break;
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

static int run_gain_tests(void);

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
    return run_gain_tests();
}

// ---------------------------------------------------------------------------
// Per-channel gain tests (assert-based; non-zero exit on failure).
// A DC (+1.0) input on a single channel makes the output sample exactly
// gain*coeff (one multiply, no accumulation), so expectations are exact
// float32 bit patterns.
// ---------------------------------------------------------------------------

static int gain_failures = 0;

#define GCHECK(cond, label) do { \
        printf("%s: %s\n", (cond) ? "PASS" : "FAIL", label); \
        if (!(cond)) gain_failures++; \
    } while (0)

static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

#define TFRAMES 8

static void dc_run(int nch, int ch, float dc,
        const spatial_downmix_config_t* cfg, float outLR[2]) {
    float in[TFRAMES * 8];
    float out[TFRAMES * 2];
    for (int i = 0; i < TFRAMES * 8; i++) {
        in[i] = 0.0f;
    }
    for (int f = 0; f < TFRAMES; f++) {
        in[f * nch + ch] = dc;
    }
    spatial_downmix_ctx_t* ctx = spatial_downmix_create(cfg);
    spatial_downmix_process(ctx, in, out, TFRAMES);
    spatial_downmix_destroy(ctx);
    outLR[0] = out[0];
    outLR[1] = out[1];
}

static void test_default_matrix_bits_5_1(void) {
    printf("\n=== Test: 5.1 default matrix bits ===\n");
    spatial_downmix_config_t cfg;
    spatial_downmix_get_default_config_5_1(&cfg);
    float lr[2];
    // channel order: FL FR FC LFE SL SR
    dc_run(6, 0, 1.0f, &cfg, lr);
    GCHECK(fbits(lr[0]) == 0x3f800000u && lr[1] == 0.0f, "5.1 FL unity");
    dc_run(6, 1, 1.0f, &cfg, lr);
    GCHECK(fbits(lr[1]) == 0x3f800000u && lr[0] == 0.0f, "5.1 FR unity");
    dc_run(6, 2, 1.0f, &cfg, lr);
    GCHECK(fbits(lr[0]) == 0x3f22eadau && lr[0] == lr[1],
            "5.1 center 0.9*0.7071");
    dc_run(6, 4, 1.0f, &cfg, lr);
    GCHECK(lr[0] == lr[1] && fbits(lr[0]) == 0x3ec71f0cu,
            "5.1 SL 0.55*0.7071");
    dc_run(6, 5, 1.0f, &cfg, lr);
    GCHECK(lr[0] == lr[1] && fbits(lr[0]) == 0x3ec71f0cu,
            "5.1 SR 0.55*0.7071");
    dc_run(6, 3, 1.0f, &cfg, lr);
    GCHECK(lr[0] == lr[1] && fbits(lr[0]) == 0x3e800000u,
            "5.1 LFE 0.25");
}

static void test_default_matrix_bits_7_1(void) {
    printf("\n=== Test: 7.1 default matrix bits ===\n");
    spatial_downmix_config_t cfg;
    spatial_downmix_get_default_config_7_1(&cfg);
    float lr[2];
    // channel order: FL FR FC LFE SL SR BL BR
    dc_run(8, 0, 1.0f, &cfg, lr);
    GCHECK(fbits(lr[0]) == 0x3f800000u && lr[1] == 0.0f, "7.1 FL unity");
    dc_run(8, 2, 1.0f, &cfg, lr);
    GCHECK(lr[0] == lr[1] && fbits(lr[0]) == 0x3f09931fu,
            "7.1 center 0.76*0.7071");
    dc_run(8, 4, 1.0f, &cfg, lr);
    GCHECK(lr[0] == lr[1] && fbits(lr[0]) == 0x3f0d31f0u,
            "7.1 SL 0.78*0.7071");
    dc_run(8, 5, 1.0f, &cfg, lr);
    GCHECK(lr[0] == lr[1] && fbits(lr[0]) == 0x3f0d31f0u,
            "7.1 SR 0.78*0.7071");
    dc_run(8, 6, 1.0f, &cfg, lr);
    GCHECK(lr[0] == lr[1] && fbits(lr[0]) == 0x3ee076c7u,
            "7.1 BL 0.62*0.7071");
    dc_run(8, 7, 1.0f, &cfg, lr);
    GCHECK(lr[0] == lr[1] && fbits(lr[0]) == 0x3ee076c7u,
            "7.1 BR 0.62*0.7071");
    dc_run(8, 3, 1.0f, &cfg, lr);
    GCHECK(lr[0] == lr[1] && fbits(lr[0]) == 0x3cf5c28fu,
            "7.1 LFE 0.03");
}

static void test_per_channel_mute_5_1(void) {
    printf("\n=== Test: 5.1 per-channel mute ===\n");
    // input channel index -> gain muting it (struct field set below)
    for (int ch = 0; ch < 6; ch++) {
        spatial_downmix_config_t cfg;
        spatial_downmix_get_default_config_5_1(&cfg);
        switch (ch) {
            case 0: cfg.gains_5_1.left = 0.0f; break;
            case 1: cfg.gains_5_1.right = 0.0f; break;
            case 2: cfg.gains_5_1.center = 0.0f; break;
            case 3: cfg.gains_5_1.lfe = 0.0f; break;
            case 4: cfg.gains_5_1.surround_left = 0.0f; break;
            case 5: cfg.gains_5_1.surround_right = 0.0f; break;
        }
        float lr[2];
        dc_run(6, ch, 1.0f, &cfg, lr);
        char label[64];
        snprintf(label, sizeof(label), "5.1 ch%d muted -> silent", ch);
        GCHECK(lr[0] == 0.0f && lr[1] == 0.0f, label);
    }
}

static void test_per_channel_mute_7_1(void) {
    printf("\n=== Test: 7.1 per-channel mute ===\n");
    for (int ch = 0; ch < 8; ch++) {
        spatial_downmix_config_t cfg;
        spatial_downmix_get_default_config_7_1(&cfg);
        switch (ch) {
            case 0: cfg.gains_7_1.left = 0.0f; break;
            case 1: cfg.gains_7_1.right = 0.0f; break;
            case 2: cfg.gains_7_1.center = 0.0f; break;
            case 3: cfg.gains_7_1.lfe = 0.0f; break;
            case 4: cfg.gains_7_1.side_left = 0.0f; break;
            case 5: cfg.gains_7_1.side_right = 0.0f; break;
            case 6: cfg.gains_7_1.rear_left = 0.0f; break;
            case 7: cfg.gains_7_1.rear_right = 0.0f; break;
        }
        float lr[2];
        dc_run(8, ch, 1.0f, &cfg, lr);
        char label[64];
        snprintf(label, sizeof(label), "7.1 ch%d muted -> silent", ch);
        GCHECK(lr[0] == 0.0f && lr[1] == 0.0f, label);
    }
}

static void test_half_gain_exact(void) {
    printf("\n=== Test: 0.5 gain is exactly half ===\n");
    spatial_downmix_config_t full, half;
    spatial_downmix_get_default_config_5_1(&full);
    spatial_downmix_get_default_config_5_1(&half);
    half.gains_5_1.center = 0.5f * full.gains_5_1.center;
    float a[2], b[2];
    dc_run(6, 2, 1.0f, &full, a);
    dc_run(6, 2, 1.0f, &half, b);
    GCHECK(b[0] * 2.0f == a[0] && b[1] * 2.0f == a[1],
            "5.1 center half-gain exact");
}

static void test_symmetry(void) {
    printf("\n=== Test: L/R symmetry ===\n");
    spatial_downmix_config_t cfg;
    spatial_downmix_get_default_config_5_1(&cfg);
    cfg.gains_5_1.surround_left = 0.4f;
    cfg.gains_5_1.surround_right = 0.4f;
    float in[TFRAMES * 6];
    float out[TFRAMES * 2];
    for (int i = 0; i < TFRAMES * 6; i++) {
        in[i] = 0.0f;
    }
    for (int f = 0; f < TFRAMES; f++) {
        in[f * 6 + 4] = 1.0f;
        in[f * 6 + 5] = 1.0f;
    }
    spatial_downmix_ctx_t* ctx = spatial_downmix_create(&cfg);
    spatial_downmix_process(ctx, in, out, TFRAMES);
    spatial_downmix_destroy(ctx);
    int mirror = 1;
    for (int f = 0; f < TFRAMES; f++) {
        if (out[f * 2] != out[f * 2 + 1]) {
            mirror = 0;
            break;
        }
    }
    GCHECK(mirror, "5.1 SL==SR gains give L/R mirror");
}

static void test_combined_gains(void) {
    printf("\n=== Test: combined gains superpose ===\n");
    spatial_downmix_config_t cfg;
    spatial_downmix_get_default_config_5_1(&cfg);
    float solo_c[2], solo_s[2], both[2];
    {
        spatial_downmix_config_t c = cfg;
        float in[TFRAMES * 6];
        float out[TFRAMES * 2];
        for (int i = 0; i < TFRAMES * 6; i++) {
            in[i] = 0.0f;
        }
        for (int f = 0; f < TFRAMES; f++) {
            in[f * 6 + 2] = 1.0f;
        }
        spatial_downmix_ctx_t* ctx = spatial_downmix_create(&c);
        spatial_downmix_process(ctx, in, out, TFRAMES);
        spatial_downmix_destroy(ctx);
        solo_c[0] = out[0];
        solo_c[1] = out[1];
    }
    {
        spatial_downmix_config_t c = cfg;
        c.gains_5_1.surround_left = 0.25f;
        c.gains_5_1.surround_right = 0.0f;
        float in[TFRAMES * 6];
        float out[TFRAMES * 2];
        for (int i = 0; i < TFRAMES * 6; i++) {
            in[i] = 0.0f;
        }
        for (int f = 0; f < TFRAMES; f++) {
            in[f * 6 + 4] = 1.0f;
        }
        spatial_downmix_ctx_t* ctx = spatial_downmix_create(&c);
        spatial_downmix_process(ctx, in, out, TFRAMES);
        spatial_downmix_destroy(ctx);
        solo_s[0] = out[0];
        solo_s[1] = out[1];
    }
    {
        spatial_downmix_config_t c = cfg;
        c.gains_5_1.surround_left = 0.25f;
        c.gains_5_1.surround_right = 0.0f;
        float in[TFRAMES * 6];
        float out[TFRAMES * 2];
        for (int i = 0; i < TFRAMES * 6; i++) {
            in[i] = 0.0f;
        }
        for (int f = 0; f < TFRAMES; f++) {
            in[f * 6 + 2] = 1.0f;
            in[f * 6 + 4] = 1.0f;
        }
        spatial_downmix_ctx_t* ctx = spatial_downmix_create(&c);
        spatial_downmix_process(ctx, in, out, TFRAMES);
        spatial_downmix_destroy(ctx);
        both[0] = out[0];
        both[1] = out[1];
    }
    GCHECK(both[0] == solo_c[0] + solo_s[0]
            && both[1] == solo_c[1] + solo_s[1],
            "5.1 C + SL combine exactly");
}

static void test_layout_independence(void) {
    printf("\n=== Test: 5.1/7.1 gain independence ===\n");
    spatial_downmix_config_t cfg;
    spatial_downmix_get_default_config_5_1(&cfg);
    float ref[2], mod[2];
    dc_run(6, 2, 1.0f, &cfg, ref);
    cfg.gains_7_1.side_left = 7.7f;
    cfg.gains_7_1.rear_right = 7.7f;
    dc_run(6, 2, 1.0f, &cfg, mod);
    GCHECK(mod[0] == ref[0] && mod[1] == ref[1],
            "5.1 output invariant to 7.1 gains");
    spatial_downmix_get_default_config_7_1(&cfg);
    dc_run(8, 4, 1.0f, &cfg, ref);
    cfg.gains_5_1.surround_left = 7.7f;
    dc_run(8, 4, 1.0f, &cfg, mod);
    GCHECK(mod[0] == ref[0] && mod[1] == ref[1],
            "7.1 output invariant to 5.1 gains");
}

static void test_validate_gain(void) {
    printf("\n=== Test: gain validation policy ===\n");
    float nan_v = 0.0f / 0.0f;
    float inf_v = 1.0f / 0.0f;
    GCHECK(!spatial_property_validate_gain(nan_v), "NaN rejected");
    GCHECK(!spatial_property_validate_gain(inf_v), "Inf rejected");
    GCHECK(!spatial_property_validate_gain(-1.0f), "negative rejected");
    GCHECK(spatial_property_validate_gain(0.0f), "0.0 allowed (mute)");
    GCHECK(spatial_property_validate_gain(2.0f), "2.0 allowed (+6dB)");
}

static void tset(const char* key, const char* val) {
#ifdef _WIN32
    char buf[256];
    snprintf(buf, sizeof(buf), "%s=%s", key, val);
    _putenv(buf);
#else
    setenv(key, val, 1);
#endif
}

static void tunset(const char* key) {
#ifdef _WIN32
    char buf[256];
    snprintf(buf, sizeof(buf), "%s=", key);
    _putenv(buf);
#else
    unsetenv(key);
#endif
}

static void test_property_fallback(void) {
    printf("\n=== Test: coarse/fine property precedence ===\n");
    spatial_property_ctx_t* pctx = spatial_property_create();
    spatial_config_t cfg;
    // Clean slate: none of the fine keys exist in a host env.
    tunset("persist.vendor.spatialdm.5_1.surround");
    tunset("persist.vendor.spatialdm.5_1.surround_left");
    tunset("persist.vendor.spatialdm.5_1.surround_right");
    spatial_property_read_all(pctx, &cfg);
    GCHECK(cfg.gains_5_1[3] == 0.55f && cfg.gains_5_1[4] == 0.55f,
            "unset -> default pair");
    tset("persist.vendor.spatialdm.5_1.surround", "0.3");
    spatial_property_read_all(pctx, &cfg);
    GCHECK(cfg.gains_5_1[3] == 0.3f && cfg.gains_5_1[4] == 0.3f,
            "coarse surround fans out to SL+SR");
    tset("persist.vendor.spatialdm.5_1.surround_left", "0.0");
    spatial_property_read_all(pctx, &cfg);
    GCHECK(cfg.gains_5_1[3] == 0.0f && cfg.gains_5_1[4] == 0.3f,
            "fine SL overrides, SR keeps coarse");
    // Each refresh rebuilds from defaults: an invalid coarse value is
    // ignored, so SR returns to default while fine SL still applies.
    tset("persist.vendor.spatialdm.5_1.surround", "abc");
    spatial_property_read_all(pctx, &cfg);
    GCHECK(cfg.gains_5_1[3] == 0.0f && cfg.gains_5_1[4] == 0.55f,
            "invalid coarse ignored, fine SL kept, SR default");
    tunset("persist.vendor.spatialdm.5_1.surround");
    tset("persist.vendor.spatialdm.5_1.surround_right", "nan");
    spatial_property_read_all(pctx, &cfg);
    GCHECK(cfg.gains_5_1[4] == 0.55f,
            "NaN fine falls back to default");
    tunset("persist.vendor.spatialdm.5_1.surround_left");
    tunset("persist.vendor.spatialdm.5_1.surround_right");
    spatial_property_destroy(pctx);
}

static int run_gain_tests(void) {
    test_default_matrix_bits_5_1();
    test_default_matrix_bits_7_1();
    test_per_channel_mute_5_1();
    test_per_channel_mute_7_1();
    test_half_gain_exact();
    test_symmetry();
    test_combined_gains();
    test_layout_independence();
    test_validate_gain();
    test_property_fallback();
    printf("\ngain tests: %s (%d failures)\n",
            gain_failures == 0 ? "ALL PASS" : "FAILURES",
            gain_failures);
    return gain_failures;
}
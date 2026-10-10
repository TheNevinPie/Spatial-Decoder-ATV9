// pcm_resample_test.c — capacity-safety tests for spatial_pcm_converter.
// No FFmpeg, no files, no properties: pure converter behavior with
// guard-canary planes. Exit code = failure count (0 = all pass).
#include "spatial_pcm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static int failures = 0;
#define CHECK(cond, label) do { \
        printf("%s: %s\n", (cond) ? "PASS" : "FAIL", label); \
        if (!(cond)) failures++; \
    } while (0)

#define GUARD_FLOATS 64
#define GUARD_PATTERN 0xA5A5A5A5u

typedef struct {
    float* planes[8];
    uint8_t* ptrs[8];
    int cap;  // usable floats per plane (guards excluded)
} guard_planes_t;

static guard_planes_t* planes_alloc(int nch, int cap) {
    guard_planes_t* g = (guard_planes_t*)calloc(1, sizeof(*g));
    if (!g) {
        return NULL;
    }
    g->cap = cap;
    for (int c = 0; c < 8; c++) {
        float* p = (float*)malloc(
                (size_t)(cap + 2 * GUARD_FLOATS) * sizeof(float));
        if (!p) {
            for (int k = 0; k < c; k++) {
                free(g->planes[k]);
            }
            free(g);
            return NULL;
        }
        uint32_t* w = (uint32_t*)p;
        for (int i = 0; i < GUARD_FLOATS; i++) {
            w[i] = GUARD_PATTERN;
            w[GUARD_FLOATS + cap + i] = GUARD_PATTERN;
        }
        g->planes[c] = p;
        g->ptrs[c] = (uint8_t*)(p + GUARD_FLOATS);
        (void)nch;
    }
    return g;
}

static int planes_guards_ok(const guard_planes_t* g) {
    for (int c = 0; c < 8; c++) {
        const uint32_t* w = (const uint32_t*)g->planes[c];
        for (int i = 0; i < GUARD_FLOATS; i++) {
            if (w[i] != GUARD_PATTERN
                    || w[GUARD_FLOATS + g->cap + i] != GUARD_PATTERN) {
                return 0;
            }
        }
    }
    return 1;
}

static void planes_free(guard_planes_t* g) {
    if (!g) {
        return;
    }
    for (int c = 0; c < 8; c++) {
        free(g->planes[c]);
    }
    free(g);
}

static void make_format(spatial_pcm_format_t* fmt, int fmt_id, int rate,
        int nch) {
    memset(fmt, 0, sizeof(*fmt));
    fmt->format = (spatial_sample_fmt_t)fmt_id;
    fmt->sample_rate = rate;
    fmt->layout = nch == 8 ? 2 : (nch == 6 ? 1 : 0);
    fmt->num_channels = (uint8_t)nch;
    for (int i = 0; i < nch && i < 8; i++) {
        fmt->channel_map[i] = (uint8_t)i;
    }
}

static spatial_pcm_converter_t* make_conv(int in_fmt, int in_rate,
        int out_fmt, int out_rate, int nch) {
    spatial_pcm_format_t inf, outf;
    make_format(&inf, in_fmt, in_rate, nch);
    make_format(&outf, out_fmt, out_rate, nch);
    return spatial_pcm_converter_create(&inf, &outf);
}

static void fill_dc_fltp(uint8_t* const* ptrs, int nch, int nb, float v) {
    for (int c = 0; c < nch; c++) {
        float* p = (float*)ptrs[c];
        for (int i = 0; i < nb; i++) {
            p[i] = v;
        }
    }
}

// The original overflow case: 44.1 -> 48 kHz, 4096 in, needs ~4459 out.
static void test_overflow_case(void) {
    printf("\n=== Test: 44.1k 4096-frame overflow case ===\n");
    spatial_pcm_converter_t* conv =
            make_conv(6, 44100, 6, 48000, 6);
    guard_planes_t* in = planes_alloc(6, 4096);
    guard_planes_t* out = planes_alloc(6, 4096);
    fill_dc_fltp(in->ptrs, 6, 4096, 0.5f);

    // Legacy entry must refuse (clean error) instead of overflowing.
    int r = spatial_pcm_converter_process(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 4096);
    CHECK(r != 0, "legacy rejects oversize resampled output");
    CHECK(planes_guards_ok(out), "legacy: output guards intact");

    // Capped entry truncates safely at capacity and reports it.
    int produced = -1;
    r = spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 4096,
            0, 4096, &produced);
    CHECK(r == 1, "capped reports truncation");
    CHECK(produced > 0 && produced <= 4096, "produced within capacity");
    CHECK(planes_guards_ok(out), "capped: output guards intact");
    CHECK(planes_guards_ok(in), "input guards intact");

    // Resume with offset: remainder is exact and concatenates bitwise
    // with a full-capacity reference run.
    guard_planes_t* part = planes_alloc(6, 8192);
    guard_planes_t* full = planes_alloc(6, 8192);
    int produced2 = -1;
    r = spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, part->ptrs, 4096,
            produced, 8192, &produced2);
    CHECK(r == 0, "resume completes");
    int full_n = -1;
    r = spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, full->ptrs, 4096,
            0, 8192, &full_n);
    int same = (r == 0 && full_n == produced + produced2);
    for (int c = 0; same && c < 6; c++) {
        const float* a = (const float*)out->ptrs[c];
        const float* b = (const float*)full->ptrs[c];
        for (int i = 0; i < produced; i++) {
            if (a[i] != b[i]) {
                same = 0;
                break;
            }
        }
        const float* a2 = (const float*)part->ptrs[c];
        for (int i = 0; same && i < produced2; i++) {
            if (a2[produced + i] != b[produced + i]) {
                same = 0;
                break;
            }
        }
    }
    CHECK(same, "split calls concatenate bitwise-exactly");
    CHECK(planes_guards_ok(part), "resume guards intact");
    CHECK(planes_guards_ok(full), "reference guards intact");

    spatial_pcm_converter_destroy(conv);
    planes_free(in);
    planes_free(out);
    planes_free(part);
    planes_free(full);
}

static void test_passthrough(void) {
    printf("\n=== Test: 48k passthrough incl. max frame ===\n");
    spatial_pcm_converter_t* conv = make_conv(6, 48000, 6, 48000, 6);
    guard_planes_t* in = planes_alloc(6, 4096);
    guard_planes_t* out = planes_alloc(6, 4096);
    fill_dc_fltp(in->ptrs, 6, 4096, 0.25f);
    int produced = -1;
    int r = spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 4096,
            0, 4096, &produced);
    CHECK(r == 0 && produced == 4096, "full passthrough, exact count");
    int exact = 1;
    for (int c = 0; c < 6 && exact; c++) {
        const float* p = (const float*)out->ptrs[c];
        for (int i = 0; i < 4096; i++) {
            if (p[i] != 0.25f) {
                exact = 0;
                break;
            }
        }
    }
    CHECK(exact, "passthrough samples bitwise exact");
    CHECK(planes_guards_ok(out), "guards intact");
    // Legacy entry agrees on fitting input.
    int rl = spatial_pcm_converter_process(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 4096);
    CHECK(rl == 0, "legacy succeeds on fitting input");
    spatial_pcm_converter_destroy(conv);
    planes_free(in);
    planes_free(out);
}

static void test_downsample_rates(void) {
    printf("\n=== Test: 96k/192k downsampling bounds ===\n");
    const int rates[2] = {96000, 192000};
    for (int k = 0; k < 2; k++) {
        spatial_pcm_converter_t* conv =
                make_conv(6, rates[k], 6, 48000, 2);
        guard_planes_t* in = planes_alloc(2, 4096);
        guard_planes_t* out = planes_alloc(2, 4096);
        fill_dc_fltp(in->ptrs, 2, 4096, 0.5f);
        int produced = -1;
        int r = spatial_pcm_converter_process_capped(conv,
                (const uint8_t* const*)in->ptrs, out->ptrs, 4096,
                0, 4096, &produced);
        char label[96];
        snprintf(label, sizeof(label), "%dk fits capacity, finite output",
                rates[k] / 1000);
        int finite = (r == 0);
        const float* p = (const float*)out->ptrs[0];
        for (int i = 0; finite && i < produced; i++) {
            float v = p[i] < 0 ? -p[i] : p[i];
            if (!(v <= 1.0f)) {
                finite = 0;
            }
        }
        CHECK(finite && planes_guards_ok(out), label);
        spatial_pcm_converter_destroy(conv);
        planes_free(in);
        planes_free(out);
    }
}

static void test_capacity_edges(void) {
    printf("\n=== Test: capacity edges ===\n");
    // 44.1k, nb chosen so need lands just over a small cap.
    spatial_pcm_converter_t* conv = make_conv(2, 44100, 2, 48000, 2);
    guard_planes_t* in = planes_alloc(2, 64);
    guard_planes_t* out = planes_alloc(2, 64);
    fill_dc_fltp(in->ptrs, 2, 64, 0.5f);
    // need = floor(64*48000/44100)+1 = 69+1 = 70.
    int produced = -1;
    int r = spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 64,
            0, 70, &produced);
    CHECK(r == 0 && produced == 70, "exact-capacity completes");
    CHECK(planes_guards_ok(out), "exact-capacity guards intact");
    produced = -1;
    r = spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 64,
            0, 69, &produced);
    CHECK(r == 1 && produced == 69, "one-short truncates, guards hold");
    CHECK(planes_guards_ok(out), "one-short guards intact");
    // Zero capacity: clean truncated signal, no writes.
    produced = -1;
    r = spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 64,
            0, 0, &produced);
    CHECK(r == 1 && produced == 0, "zero capacity truncates cleanly");
    CHECK(planes_guards_ok(out), "zero-capacity guards intact");
    // Offset past the need: vacuous success.
    produced = -1;
    r = spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 64,
            70, 4096, &produced);
    CHECK(r == 0 && produced == 0, "offset-past-need is vacuous success");
    // Empty input and bad arguments: errors, produced zeroed.
    produced = 99;
    CHECK(spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 0,
            0, 4096, &produced) == -1 && produced == 0,
            "empty input errors");
    CHECK(spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 64,
            -1, 4096, &produced) == -1 && produced == 0,
            "negative offset errors");
    CHECK(spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 64,
            0, -5, &produced) == -1 && produced == 0,
            "negative capacity errors");
    CHECK(spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 5000,
            0, 8192, &produced) == -1 && produced == 0,
            "oversize input errors");
    // NULL produced pointer is accepted.
    r = spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 64,
            0, 70, NULL);
    CHECK(r == 0, "NULL produced pointer accepted");
    spatial_pcm_converter_destroy(conv);
    planes_free(in);
    planes_free(out);
}

static void test_invalid_formats(void) {
    printf("\n=== Test: invalid formats/ratios ===\n");
    spatial_pcm_format_t inf, outf;
    memset(&inf, 0, sizeof(inf));
    memset(&outf, 0, sizeof(outf));
    inf.format = 6;
    inf.sample_rate = 0;  // corrupt: zero rate -> non-finite ratio
    inf.layout = 0;
    inf.num_channels = 2;
    outf.format = 6;
    outf.sample_rate = 48000;
    outf.layout = 0;
    outf.num_channels = 2;
    spatial_pcm_converter_t* conv =
            spatial_pcm_converter_create(&inf, &outf);
    guard_planes_t* in = planes_alloc(2, 64);
    guard_planes_t* out = planes_alloc(2, 64);
    fill_dc_fltp(in->ptrs, 2, 64, 0.5f);
    int produced = 99;
    int r = spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, out->ptrs, 64,
            0, 64, &produced);
    CHECK(r == -1 && produced == 0, "zero input rate errors cleanly");
    CHECK(planes_guards_ok(out), "guards intact on format error");
    spatial_pcm_converter_destroy(conv);
    planes_free(in);
    planes_free(out);
}

static void test_interleaved_parity(void) {
    printf("\n=== Test: interleaved output parity ===\n");
    // Same conversion to planar-float vs interleaved-float outputs must
    // agree sample-for-sample (exercises the interleaved write branches).
    spatial_pcm_converter_t* convP = make_conv(6, 44100, 6, 48000, 6);
    spatial_pcm_format_t inf, outf;
    make_format(&inf, 6, 44100, 6);
    make_format(&outf, 2, 48000, 6);
    spatial_pcm_converter_t* convI =
            spatial_pcm_converter_create(&inf, &outf);
    guard_planes_t* in = planes_alloc(6, 512);
    guard_planes_t* pl = planes_alloc(6, 1024);
    // Interleaved output packs all channels into plane 0: size it so.
    guard_planes_t* il = planes_alloc(6, 1024 * 6);
    fill_dc_fltp(in->ptrs, 6, 512, 0.3f);
    // Vary per-channel content so a channel mix-up would show.
    for (int c = 0; c < 6; c++) {
        float* p = (float*)in->ptrs[c];
        for (int i = 0; i < 512; i++) {
            p[i] = 0.1f * (float)(c + 1) + 0.0001f * (float)i;
        }
    }
    int pp = -1, pi = -1;
    int rp = spatial_pcm_converter_process_capped(convP,
            (const uint8_t* const*)in->ptrs, pl->ptrs, 512,
            0, 1024, &pp);
    int ri = spatial_pcm_converter_process_capped(convI,
            (const uint8_t* const*)in->ptrs, il->ptrs, 512,
            0, 1024, &pi);
    int same = (rp == 0 && ri == 0 && pp == pi && pp > 0);
    for (int c = 0; same && c < 6; c++) {
        const float* p = (const float*)pl->ptrs[c];
        const float* q = (const float*)il->ptrs[0];
        for (int i = 0; i < pp; i++) {
            if (p[i] != q[i * 6 + c]) {
                same = 0;
                break;
            }
        }
    }
    CHECK(same, "planar and interleaved outputs agree bitwise");
    CHECK(planes_guards_ok(il), "interleaved guards intact");
    spatial_pcm_converter_destroy(convP);
    spatial_pcm_converter_destroy(convI);
    planes_free(in);
    planes_free(pl);
    planes_free(il);
}

static void test_flush_determinism(void) {
    printf("\n=== Test: flush determinism ===\n");
    spatial_pcm_converter_t* conv = make_conv(6, 44100, 6, 48000, 6);
    guard_planes_t* in = planes_alloc(6, 512);
    guard_planes_t* a = planes_alloc(6, 1024);
    guard_planes_t* b = planes_alloc(6, 1024);
    fill_dc_fltp(in->ptrs, 6, 512, 0.3f);
    int p1 = -1, p2 = -1;
    spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, a->ptrs, 512,
            0, 1024, &p1);
    spatial_pcm_converter_flush(conv);
    spatial_pcm_converter_process_capped(conv,
            (const uint8_t* const*)in->ptrs, b->ptrs, 512,
            0, 1024, &p2);
    int same = (p1 == p2);
    for (int c = 0; same && c < 6; c++) {
        const float* pa = (const float*)a->ptrs[c];
        const float* pb = (const float*)b->ptrs[c];
        for (int i = 0; i < p1; i++) {
            if (pa[i] != pb[i]) {
                same = 0;
                break;
            }
        }
    }
    CHECK(same && p1 > 0, "flush reproduces identical output");
    spatial_pcm_converter_destroy(conv);
    planes_free(in);
    planes_free(a);
    planes_free(b);
}

int main(void) {
    printf("========================================\n");
    printf("PCM resampler capacity-safety tests\n");
    printf("========================================\n");
    test_overflow_case();
    test_passthrough();
    test_downsample_rates();
    test_capacity_edges();
    test_invalid_formats();
    test_interleaved_parity();
    test_flush_determinism();
    printf("\nTOTAL: %s (%d failures)\n",
            failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures;
}

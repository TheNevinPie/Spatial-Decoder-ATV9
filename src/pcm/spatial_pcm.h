#ifndef SPATIAL_PCM_H
#define SPATIAL_PCM_H

#include <stdint.h>
#include <stddef.h>

#include "spatial_channel_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPATIAL_SAMPLE_FMT_S16 = 0,
    SPATIAL_SAMPLE_FMT_S32 = 1,
    SPATIAL_SAMPLE_FMT_FLT = 2,
    SPATIAL_SAMPLE_FMT_DBL = 3,
    SPATIAL_SAMPLE_FMT_S16P = 4,
    SPATIAL_SAMPLE_FMT_S32P = 5,
    SPATIAL_SAMPLE_FMT_FLTP = 6,
    SPATIAL_SAMPLE_FMT_DBLP = 7,
} spatial_sample_fmt_t;

typedef struct {
    spatial_sample_fmt_t format;
    int sample_rate;
    spatial_layout_t layout;
    uint8_t channel_map[8];
    uint8_t num_channels;
} spatial_pcm_format_t;

typedef struct {
    int nb_samples;
    int sample_rate;
    spatial_sample_fmt_t format;
    uint8_t channel_map[8];
    uint8_t num_channels;
} spatial_pcm_info_t;

typedef struct spatial_pcm_converter spatial_pcm_converter_t;

spatial_pcm_converter_t* spatial_pcm_converter_create(
    const spatial_pcm_format_t* input_format,
    const spatial_pcm_format_t* output_format);

void spatial_pcm_converter_destroy(spatial_pcm_converter_t* converter);

int spatial_pcm_converter_process(spatial_pcm_converter_t* converter,
                                  const uint8_t* const input_data[8],
                                  uint8_t* const output_data[8],
                                  int nb_samples);

// Capacity-aware processing. output_data planes each hold out_capacity
// output samples; production starts at out_offset (0 for a fresh frame).
// Return: 0 = all nb_samples converted (out_produced == full need);
// 1 = truncated, out_produced < need; call again with out_offset advanced
// by *out_produced and the SAME input to produce the remainder (each
// output sample depends only on the input, so split calls concatenate
// bitwise-exactly). -1 = argument/format error; nothing is written and
// *out_produced (if non-NULL) is set to 0. out_produced may be NULL.
// The legacy process() above enforces the documented 4096-sample ceiling
// and reports oversize resampled output as an error instead of
// overflowing the destination.
int spatial_pcm_converter_process_capped(spatial_pcm_converter_t* converter,
                                  const uint8_t* const input_data[8],
                                  uint8_t* const output_data[8],
                                  int nb_samples,
                                  int out_offset,
                                  int out_capacity,
                                  int* out_produced);

int spatial_pcm_converter_flush(spatial_pcm_converter_t* converter);

int spatial_pcm_format_get_bytes_per_sample(spatial_sample_fmt_t fmt);
int spatial_pcm_format_is_planar(spatial_sample_fmt_t fmt);
const char* spatial_sample_fmt_name(spatial_sample_fmt_t fmt);

spatial_layout_t spatial_layout_from_channel_count(int channels);
int spatial_layout_get_channel_count(spatial_layout_t layout);

#ifdef __cplusplus
}
#endif

#endif
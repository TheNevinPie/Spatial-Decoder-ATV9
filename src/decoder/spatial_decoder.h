#ifndef SPATIAL_DECODER_H
#define SPATIAL_DECODER_H

#include <stdint.h>
#include <stddef.h>

#include "spatial_channel_layout.h"
#include "spatial_pcm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPATIAL_DECODER_OK = 0,
    SPATIAL_DECODER_ERROR = -1,
    SPATIAL_DECODER_ERROR_INVALID_ARG = -2,
    SPATIAL_DECODER_ERROR_DECODER_NOT_FOUND = -3,
    SPATIAL_DECODER_ERROR_OPEN_FAILED = -4,
    SPATIAL_DECODER_ERROR_SEND_PACKET = -5,
    SPATIAL_DECODER_ERROR_RECEIVE_FRAME = -6,
    SPATIAL_DECODER_ERROR_EOF = -7,
    SPATIAL_DECODER_AGAIN = -11,
} spatial_decoder_status_t;

typedef enum {
    SPATIAL_CODEC_AC3 = 0,
    SPATIAL_CODEC_EAC3 = 1,
    SPATIAL_CODEC_DTS = 2,
    SPATIAL_CODEC_TRUEHD = 3,
} spatial_codec_t;

typedef struct {
    spatial_codec_t codec;
    uint32_t sample_rate;
    uint8_t channel_map[SPATIAL_CH_MAX];
    uint8_t num_channels;
} spatial_decoder_config_t;

typedef struct {
    uint8_t *data[SPATIAL_CH_MAX];
    int linesize[SPATIAL_CH_MAX];
    int nb_samples;
    int sample_rate;
    spatial_sample_fmt_t format;
    uint8_t channel_map[SPATIAL_CH_MAX];
    uint8_t num_channels;
    int64_t pts;
    int64_t duration;
} spatial_frame_t;

typedef struct spatial_decoder spatial_decoder_t;

spatial_decoder_t* spatial_decoder_create(const spatial_decoder_config_t* config);
void spatial_decoder_destroy(spatial_decoder_t* decoder);

int spatial_decoder_open(spatial_decoder_t* decoder);
int spatial_decoder_close(spatial_decoder_t* decoder);

int spatial_decoder_send_packet(spatial_decoder_t* decoder, const uint8_t* data, size_t size, int64_t pts);
int spatial_decoder_receive_frame(spatial_decoder_t* decoder, spatial_frame_t* frame);

int spatial_decoder_flush(spatial_decoder_t* decoder);

const char* spatial_decoder_strerror(int err);
const char* spatial_decoder_get_codec_name(spatial_codec_t codec);

#ifdef __cplusplus
}
#endif

#endif
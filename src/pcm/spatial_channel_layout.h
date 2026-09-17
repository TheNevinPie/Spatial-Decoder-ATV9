#ifndef SPATIAL_CHANNEL_LAYOUT_H
#define SPATIAL_CHANNEL_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPATIAL_CH_FL = 0,
    SPATIAL_CH_FR = 1,
    SPATIAL_CH_FC = 2,
    SPATIAL_CH_LFE = 3,
    SPATIAL_CH_SL = 4,
    SPATIAL_CH_SR = 5,
    SPATIAL_CH_BL = 6,
    SPATIAL_CH_BR = 7,
    SPATIAL_CH_MAX = 8,
} spatial_channel_t;

typedef enum {
    SPATIAL_LAYOUT_STEREO = 0,
    SPATIAL_LAYOUT_5_1 = 1,
    SPATIAL_LAYOUT_7_1 = 2,
    SPATIAL_LAYOUT_UNKNOWN = 255,
} spatial_layout_t;

typedef struct {
    spatial_channel_t channels[8];
    int num_channels;
    spatial_layout_t layout;
} spatial_channel_map_t;

int spatial_layout_from_ffmpeg(uint64_t ffmpeg_layout, spatial_layout_t* out_layout);
int spatial_layout_to_ffmpeg(spatial_layout_t layout, uint64_t* out_ffmpeg_layout);

int spatial_channel_map_create(spatial_layout_t layout, spatial_channel_map_t* out_map);
int spatial_channel_map_from_ffmpeg(uint64_t ffmpeg_layout, spatial_channel_map_t* out_map);

int spatial_channel_map_remap(const spatial_channel_map_t* src, const spatial_channel_map_t* dst, int* remap_indices);

bool spatial_channel_map_equal(const spatial_channel_map_t* a, const spatial_channel_map_t* b);
void spatial_channel_map_print(const spatial_channel_map_t* map);

const char* spatial_channel_name(spatial_channel_t ch);
const char* spatial_layout_name(spatial_layout_t layout);

#ifdef __cplusplus
}
#endif

#endif
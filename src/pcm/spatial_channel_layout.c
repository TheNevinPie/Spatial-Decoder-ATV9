#include "spatial_channel_layout.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define FF_CH_FL        (1ULL << 0)
#define FF_CH_FR        (1ULL << 1)
#define FF_CH_FC        (1ULL << 2)
#define FF_CH_LFE       (1ULL << 3)
#define FF_CH_BL        (1ULL << 4)
#define FF_CH_BR        (1ULL << 5)
#define FF_CH_FLC       (1ULL << 6)
#define FF_CH_FRC       (1ULL << 7)
#define FF_CH_BC        (1ULL << 8)
#define FF_CH_SL        (1ULL << 9)
#define FF_CH_SR        (1ULL << 10)
#define FF_CH_TC        (1ULL << 11)
#define FF_CH_TFL       (1ULL << 12)
#define FF_CH_TFR       (1ULL << 13)
#define FF_CH_TFC       (1ULL << 14)
#define FF_CH_TBL       (1ULL << 15)
#define FF_CH_TBR       (1ULL << 16)
#define FF_CH_LFE2      (1ULL << 17)

static const struct {
    uint64_t ffmpeg_layout;
    spatial_layout_t layout;
} known_layouts[] = {
    { FF_CH_FL | FF_CH_FR, 0 },
    { FF_CH_FL | FF_CH_FR | FF_CH_FC | FF_CH_LFE | FF_CH_SL | FF_CH_SR, 1 },
    { FF_CH_FL | FF_CH_FR | FF_CH_FC | FF_CH_LFE | FF_CH_SL | FF_CH_SR | FF_CH_BL | FF_CH_BR, 2 },
};

static const struct {
    spatial_channel_t spatial;
    uint64_t ffmpeg;
} channel_map[] = {
    { 0, 1ULL << 0 },
    { 1, 1ULL << 1 },
    { 2, 1ULL << 2 },
    { 3, 1ULL << 3 },
    { 4, 1ULL << 9 },
    { 5, 1ULL << 10 },
    { 6, 1ULL << 4 },
    { 7, 1ULL << 5 },
};

int spatial_layout_from_ffmpeg(uint64_t ffmpeg_layout, spatial_layout_t* out_layout) {
    if (!out_layout) return -1;
    
    for (size_t i = 0; i < sizeof(known_layouts)/sizeof(known_layouts[0]); i++) {
        if (known_layouts[i].ffmpeg_layout == ffmpeg_layout) {
            *out_layout = known_layouts[i].layout;
            return 0;
        }
    }
    *out_layout = 255;
    return -1;
}

int spatial_layout_to_ffmpeg(spatial_layout_t layout, uint64_t* out_ffmpeg_layout) {
    if (!out_ffmpeg_layout) return -1;
    
    for (size_t i = 0; i < sizeof(known_layouts)/sizeof(known_layouts[0]); i++) {
        if (known_layouts[i].layout == layout) {
            *out_ffmpeg_layout = known_layouts[i].ffmpeg_layout;
            return 0;
        }
    }
    return -1;
}

int spatial_channel_map_create(spatial_layout_t layout, spatial_channel_map_t* out_map) {
    if (!out_map) return -1;
    
    memset(out_map, 0, sizeof(spatial_channel_map_t));
    out_map->layout = layout;
    
    switch (layout) {
        case 0:
            out_map->channels[0] = 0;
            out_map->channels[1] = 1;
            out_map->num_channels = 2;
            break;
        case 1:
            out_map->channels[0] = 0;
            out_map->channels[1] = 1;
            out_map->channels[2] = 2;
            out_map->channels[3] = 3;
            out_map->channels[4] = 4;
            out_map->channels[5] = 5;
            out_map->num_channels = 6;
            break;
        case 2:
            out_map->channels[0] = 0;
            out_map->channels[1] = 1;
            out_map->channels[2] = 2;
            out_map->channels[3] = 3;
            out_map->channels[4] = 4;
            out_map->channels[5] = 5;
            out_map->channels[6] = 6;
            out_map->channels[7] = 7;
            out_map->num_channels = 8;
            break;
        default:
            out_map->num_channels = 0;
            out_map->layout = 255;
            return -1;
    }
    return 0;
}

int spatial_channel_map_from_ffmpeg(uint64_t ffmpeg_layout, spatial_channel_map_t* out_map) {
    if (!out_map) return -1;
    
    spatial_layout_t layout;
    if (spatial_layout_from_ffmpeg(ffmpeg_layout, &layout) != 0) {
        return -1;
    }
    return spatial_channel_map_create(layout, out_map);
}

int spatial_channel_map_remap(const spatial_channel_map_t* src, const spatial_channel_map_t* dst, int* remap_indices) {
    if (!src || !dst || !remap_indices) return -1;
    
    for (int i = 0; i < src->num_channels; i++) {
        remap_indices[i] = -1;
        for (int j = 0; j < dst->num_channels; j++) {
            if (src->channels[i] == dst->channels[j]) {
                remap_indices[i] = j;
                break;
            }
        }
    }
    return 0;
}

bool spatial_channel_map_equal(const spatial_channel_map_t* a, const spatial_channel_map_t* b) {
    if (!a || !b) return false;
    if (a->layout != b->layout || a->num_channels != b->num_channels) return false;
    return memcmp(a->channels, b->channels, a->num_channels * sizeof(spatial_channel_t)) == 0;
}

void spatial_channel_map_print(const spatial_channel_map_t* map) {
    if (!map) return;
    printf("Layout: %s (%d channels)\n", spatial_layout_name(map->layout), map->num_channels);
    for (int i = 0; i < map->num_channels; i++) {
        printf("  [%d] %s\n", i, spatial_channel_name(map->channels[i]));
    }
}

const char* spatial_channel_name(spatial_channel_t ch) {
    static const char* names[] = {"FL", "FR", "FC", "LFE", "SL", "SR", "BL", "BR"};
    if (ch >= 0 && ch < 8) return names[ch];
    return "UNK";
}

const char* spatial_layout_name(spatial_layout_t layout) {
    static const char* names[] = {"STEREO", "5.1", "7.1"};
    if (layout >= 0 && layout < 3) return names[layout];
    return "UNKNOWN";
}
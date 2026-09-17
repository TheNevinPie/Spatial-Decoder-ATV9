#include "spatial_decoder.h"
#include "spatial_downmix.h"
#include "spatial_pcm.h"
#include "spatial_channel_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void analyze_stereo(const int16_t* data, int frames, float* rms_l, float* rms_r, float* peak_l, float* peak_r) {
    float sum_l = 0, sum_r = 0;
    float p_l = 0, p_r = 0;
    
    for (int i = 0; i < frames; i++) {
        float l = data[i * 2] / 32768.0f;
        float r = data[i * 2 + 1] / 32768.0f;
        sum_l += l * l;
        sum_r += r * r;
        if (fabsf(l) > p_l) p_l = fabsf(l);
        if (fabsf(r) > p_r) p_r = fabsf(r);
    }
    
    *rms_l = sqrtf(sum_l / frames);
    *rms_r = sqrtf(sum_r / frames);
    *peak_l = p_l;
    *peak_r = p_r;
}

static int read_file(const char* path, uint8_t** out_data, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t* data = malloc(size);
    if (!data) {
        fclose(f);
        return -1;
    }
    
    if (fread(data, 1, size, f) != size) {
        free(data);
        fclose(f);
        return -1;
    }
    
    fclose(f);
    *out_data = data;
    *out_size = size;
    return 0;
}

static void test_decoder_format(const char* filepath, spatial_codec_t codec, const char* name) {
    printf("\n=== Testing %s (%s) ===\n", name, filepath);
    
    uint8_t* data = NULL;
    size_t size = 0;
    if (read_file(filepath, &data, &size) < 0) {
        printf("Failed to read %s\n", filepath);
        return;
    }
    
    spatial_decoder_config_t config = {0};
    config.codec = codec;
    config.sample_rate = 48000;
    config.num_channels = 6;
    config.channel_map[0] = 0;
    config.channel_map[1] = 1;
    config.channel_map[2] = 2;
    config.channel_map[3] = 3;
    config.channel_map[4] = 4;
    config.channel_map[5] = 5;
    
    spatial_decoder_t* decoder = spatial_decoder_create(&config);
    if (!decoder) {
        printf("Failed to create decoder\n");
        free(data);
        return;
    }
    
    int ret = spatial_decoder_open(decoder);
    if (ret != 0) {
        printf("Failed to open decoder: %s\n", spatial_decoder_strerror(ret));
        spatial_decoder_destroy(decoder);
        free(data);
        return;
    }
    
    spatial_downmix_config_t dm_config;
    spatial_downmix_get_default_config_5_1(&dm_config);
    dm_config.gains_5_1.center = 1.0f;
    dm_config.gains_5_1.surround = 1.0f;
    dm_config.gains_5_1.lfe = 1.0f;
    
    spatial_downmix_ctx_t* downmix = spatial_downmix_create(&dm_config);
    
    spatial_pcm_format_t dec_format = {0};
    dec_format.format = 6;
    dec_format.sample_rate = 48000;
    dec_format.layout = 1;
    dec_format.num_channels = 6;
    dec_format.channel_map[0] = 0;
    dec_format.channel_map[1] = 1;
    dec_format.channel_map[2] = 2;
    dec_format.channel_map[3] = 3;
    dec_format.channel_map[4] = 4;
    dec_format.channel_map[5] = 5;
    
    spatial_pcm_format_t dm_format = {0};
    dm_format.format = 6;
    dm_format.sample_rate = 48000;
    dm_format.layout = 1;
    dm_format.num_channels = 6;
    dm_format.channel_map[0] = 0;
    dm_format.channel_map[1] = 1;
    dm_format.channel_map[2] = 2;
    dm_format.channel_map[3] = 3;
    dm_format.channel_map[4] = 4;
    dm_format.channel_map[5] = 5;
    
    spatial_pcm_converter_t* pcm_conv = spatial_pcm_converter_create(&dec_format, &dm_format);
    
    spatial_frame_t frame = {0};
    int ret = spatial_decoder_send_packet(decoder, data, size, 0);
    if (ret < 0) {
        printf("Failed to send packet: %s\n", spatial_decoder_strerror(ret));
        goto cleanup;
    }
    
    float frames[8][48000];
    float* frame_ptrs[8];
    for (int i = 0; i < 8; i++) frame_ptrs[i] = frames[i];
    
    int total_frames = 0;
    float max_peak_l = 0, max_peak_r = 0;
    float rms_l = 0, rms_r = 0;
    
    spatial_frame_t frame_out = {0};
    int total_decoded = 0;
    
    while (1) {
        int ret = spatial_decoder_receive_frame(decoder, &frame_out);
        if (ret == -11) {
            break;
        } else if (ret < 0) {
            if (ret != -7) {
                printf("Decode error: %s\n", spatial_decoder_strerror(ret));
            }
            break;
        }
        
        int nb_samples = frame_out.nb_samples;
        total_decoded += nb_samples;
        
        uint8_t* planar_ptrs[8];
        for (int ch = 0; ch < 6; ch++) {
            planar_ptrs[ch] = frame_out.data[ch];
        }
        
        spatial_pcm_converter_process(pcm_conv, (const uint8_t* const*)planar_ptrs,
                                     (uint8_t**)frame_ptrs, frame_out.nb_samples);
        
        int16_t stereo_out[48000];
        int out_samples = frame_out.nb_samples * 2;
        
        spatial_pcm_format_t stereo_fmt = {0};
        stereo_fmt.format = 0;
        stereo_fmt.sample_rate = 48000;
        stereo_fmt.layout = 0;
        stereo_fmt.num_channels = 2;
        
        spatial_pcm_converter_t* to_stereo = spatial_pcm_converter_create(&dm_format, &stereo_fmt);
        uint8_t* stereo_ptrs[2] = {(uint8_t*)stereo_out, (uint8_t*)(stereo_out + out_samples / 2)};
        spatial_pcm_converter_process(to_stereo, (const uint8_t* const*)frame_ptrs,
                                     (uint8_t**)stereo_ptrs, frame_out.nb_samples);
        spatial_pcm_converter_destroy(to_stereo);
        
        float rms_l, rms_r, peak_l, peak_r;
        analyze_stereo(stereo_out, frame_out.nb_samples, &rms_l, &rms_r, &rms_l, &rms_r);
        
        if (rms_l > 0) rms_l = (rms_l * total_frames + rms_l * frame_out.nb_samples) / (total_frames + frame_out.nb_samples);
        if (rms_r > 0) rms_r = (rms_r * total_frames + rms_r * frame_out.nb_samples) / (total_frames + frame_out.nb_samples);
        
        total_frames += frame_out.nb_samples;
    }
    
    if (total_decoded > 0) {
        printf("  Decoded: %d frames\n", total_decoded);
        printf("  Channels: %d, Sample rate: %d Hz\n", 6, 48000);
        printf("  Format: FLTP\n");
    }
    
cleanup:
    if (pcm_conv) spatial_pcm_converter_destroy(pcm_conv);
    if (downmix) spatial_downmix_destroy(downmix);
    spatial_decoder_destroy(decoder);
    free(data);
}

int main(int argc, char** argv) {
    printf("========================================\n");
    printf("Spatial Decoder - FFmpeg Backend Test\n");
    printf("========================================\n");
    
    if (argc < 2) {
        printf("Usage: %s <test_audio_dir>\n", argv[0]);
        printf("Expected files:\n");
        printf("  test_ac3_51_chid.ac3\n");
        printf("  test_eac3_51_chid.ec3\n");
        printf("  test_eac3_71_chid.ec3\n");
        printf("  test_dts_51_chid.dts\n");
        printf("  test_truehd_51_chid.thd\n");
        return 1;
    }
    
    char path[512];
    
    snprintf(path, sizeof(path), "%s/test_ac3_51_chid.ac3", argv[1]);
    test_decoder_format(path, 0, "AC-3 5.1");
    
    snprintf(path, sizeof(path), "%s/test_eac3_51_chid.ec3", argv[1]);
    test_decoder_format(path, 1, "E-AC-3 5.1");
    
    snprintf(path, sizeof(path), "%s/test_eac3_71_chid.ec3", argv[1]);
    test_decoder_format(path, 1, "E-AC-3 7.1");
    
    snprintf(path, sizeof(path), "%s/test_dts_51_chid.dts", argv[1]);
    test_decoder_format(path, 2, "DTS 5.1");
    
    snprintf(path, sizeof(path), "%s/test_truehd_51_chid.thd", argv[1]);
    test_decoder_format(path, 3, "TrueHD 5.1");
    
    printf("\n=== All Tests Complete ===\n");
    return 0;
}
#include "spatial_decoder.h"
#include "spatial_downmix.h"
#include "spatial_pcm.h"
#include "spatial_channel_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STREAM_BUF_SIZE (32 * 1024)
#define MAX_FRAME_SAMPLES 48000

typedef struct {
    float sum_sq[SPATIAL_CH_MAX];
    float peak[SPATIAL_CH_MAX];
    int frame_count;
    int total_samples;
    int total_decoded;
} decode_stats_t;

static void process_frame(spatial_pcm_converter_t* pcm_conv,
                          const spatial_frame_t* frame,
                          decode_stats_t* stats) {
    int nch = frame->num_channels;
    if (nch > SPATIAL_CH_MAX) nch = SPATIAL_CH_MAX;
    if (nch <= 0 || frame->nb_samples <= 0) return;

    float* channel_data[SPATIAL_CH_MAX] = {0};
    float out_buffers[SPATIAL_CH_MAX][MAX_FRAME_SAMPLES];
    uint8_t* out_ptrs[SPATIAL_CH_MAX] = {0};

    for (int ch = 0; ch < nch; ch++) {
        channel_data[ch] = (float*)frame->data[ch];
        out_ptrs[ch] = (uint8_t*)out_buffers[ch];
    }

    stats->frame_count++;
    if (stats->frame_count <= 3) {
        printf("  Frame %d: %d samples, sr=%d, ch=%d, fmt=%d\n",
               stats->frame_count, frame->nb_samples, frame->sample_rate,
               frame->num_channels, frame->format);
    }

    int conv_ret = spatial_pcm_converter_process(pcm_conv,
                                                 (const uint8_t* const*)channel_data,
                                                 out_ptrs,
                                                 frame->nb_samples);
    if (conv_ret == 0) {
        for (int ch = 0; ch < nch; ch++) {
            for (int i = 0; i < frame->nb_samples; i++) {
                float v = out_buffers[ch][i];
                stats->sum_sq[ch] += v * v;
                float av = fabsf(v);
                if (av > stats->peak[ch]) stats->peak[ch] = av;
            }
        }
        stats->total_samples += frame->nb_samples;
    }

    stats->total_decoded += frame->nb_samples;
}

static int drain_frames(spatial_decoder_t* decoder,
                        spatial_pcm_converter_t* pcm_conv,
                        decode_stats_t* stats) {
    spatial_frame_t frame = {0};

    while (1) {
        int ret = spatial_decoder_receive_frame(decoder, &frame);
        if (ret == SPATIAL_DECODER_AGAIN) break;
        if (ret < 0) {
            if (ret == SPATIAL_DECODER_ERROR_EOF) break;
            printf("Decode error: %s\n", spatial_decoder_strerror(ret));
            return -1;
        }
        process_frame(pcm_conv, &frame, stats);
    }

    return 0;
}

static void test_decoder_format(const char* filepath, spatial_codec_t codec, const char* name) {
    printf("\n=== Testing %s (%s) ===\n", name, filepath);

    FILE* file = fopen(filepath, "rb");
    if (!file) {
        printf("Failed to open %s\n", filepath);
        return;
    }

    spatial_decoder_config_t config = {0};
    config.codec = codec;
    config.sample_rate = 48000;
    config.num_channels = 6;
    for (int i = 0; i < 6; i++) {
        config.channel_map[i] = (uint8_t)i;
    }

    spatial_decoder_t* decoder = spatial_decoder_create(&config);
    if (!decoder) {
        printf("Failed to create decoder\n");
        fclose(file);
        return;
    }

    printf("Decoder created\n");

    int ret = spatial_decoder_open(decoder);
    if (ret != 0) {
        printf("Failed to open decoder: %s\n", spatial_decoder_strerror(ret));
        spatial_decoder_destroy(decoder);
        fclose(file);
        return;
    }

    printf("Decoder opened successfully\n");

    spatial_pcm_format_t dec_format = {0};
    dec_format.format = SPATIAL_SAMPLE_FMT_FLTP;
    dec_format.sample_rate = 48000;
    dec_format.layout = SPATIAL_LAYOUT_5_1;
    dec_format.num_channels = 6;
    for (int i = 0; i < 6; i++) {
        dec_format.channel_map[i] = (uint8_t)i;
    }

    spatial_pcm_format_t out_format = dec_format;

    spatial_pcm_converter_t* pcm_conv = spatial_pcm_converter_create(&dec_format, &out_format);
    if (!pcm_conv) {
        printf("Failed to create PCM converter\n");
        spatial_decoder_destroy(decoder);
        fclose(file);
        return;
    }

    decode_stats_t stats = {0};
    uint8_t buffer[STREAM_BUF_SIZE];
    size_t bytes_in_buffer = 0;
    size_t total_consumed = 0;
    int eof = 0;
    int send_failed = 0;

    printf("  Streaming with %d KB sliding buffer...\n", STREAM_BUF_SIZE / 1024);

    while (!send_failed) {
        if (!eof && bytes_in_buffer < STREAM_BUF_SIZE) {
            size_t space = STREAM_BUF_SIZE - bytes_in_buffer;
            size_t got = fread(buffer + bytes_in_buffer, 1, space, file);
            bytes_in_buffer += got;
            if (got == 0) {
                if (ferror(file)) {
                    printf("Read error after %zu bytes\n", total_consumed + bytes_in_buffer);
                    send_failed = 1;
                    break;
                }
                if (feof(file)) eof = 1;
            }
        }

        if (bytes_in_buffer == 0) break;

        int consumed = spatial_decoder_send_packet(decoder, buffer, bytes_in_buffer, 0);
        if (consumed < 0) {
            printf("Failed to send packet: %s\n", spatial_decoder_strerror(consumed));
            send_failed = 1;
            break;
        }

        if (consumed > 0) {
            total_consumed += (size_t)consumed;
            if ((size_t)consumed < bytes_in_buffer) {
                memmove(buffer, buffer + (size_t)consumed, bytes_in_buffer - (size_t)consumed);
            }
            bytes_in_buffer -= (size_t)consumed;
        } else if (bytes_in_buffer == STREAM_BUF_SIZE) {
            printf("  Error: parser made no progress on a full %d KB buffer (unrecognized data after %zu bytes)\n",
                   STREAM_BUF_SIZE / 1024, total_consumed);
            send_failed = 1;
            break;
        } else if (eof) {
            printf("  Warning: %zu trailing byte(s) not recognized; stopping\n", bytes_in_buffer);
            bytes_in_buffer = 0;
            break;
        }

        if (drain_frames(decoder, pcm_conv, &stats) < 0) {
            send_failed = 1;
            break;
        }
    }

    if (send_failed) {
        printf("  Decoding failed after %zu bytes\n", total_consumed);
    } else {
        printf("  File sent successfully (%zu bytes)\n", total_consumed);

        if (drain_frames(decoder, pcm_conv, &stats) < 0) {
            printf("  Failed to drain remaining frames\n");
        }

        if (stats.total_decoded > 0) {
            printf("\n=== Channel ID Verification ===\n");
            printf("Total frames: %d, Total samples: %d\n", stats.frame_count, stats.total_samples);
            printf("Format: Float planar (FLTP)\n\n");

            const char* ch_names[6] = {"FL", "FR", "C", "LFE", "SL", "SR"};
            const float expected_freqs[6] = {440.0f, 550.0f, 660.0f, 80.0f, 770.0f, 880.0f};

            printf("%-4s %10s %10s %10s %10s\n", "Ch", "Name", "RMS", "Peak", "Expected_Hz");
            printf("--------------------------------------------------\n");

            for (int ch = 0; ch < 6; ch++) {
                if (stats.total_samples > 0) {
                    float rms = sqrtf(stats.sum_sq[ch] / stats.total_samples);
                    printf("%2d  %-4s %10.6f %10.6f %10.1f\n",
                           ch, ch_names[ch], rms, stats.peak[ch], expected_freqs[ch]);
                }
            }

            printf("\nTotal decoded: %d frames (%d samples)\n", stats.frame_count, stats.total_samples);
        } else {
            printf("  No frames decoded\n");
        }

        printf("\n");
    }

    spatial_pcm_converter_destroy(pcm_conv);
    spatial_decoder_destroy(decoder);
    fclose(file);
}

int main(int argc, char** argv) {
    printf("========================================\n");
    printf("Spatial Decoder - Channel ID Verification\n");
    printf("========================================\n");
    
    if (argc < 2) {
        printf("Usage: %s <test_audio_dir>\n", argv[0]);
        printf("Expected files:\n");
        printf("  test_ac3_51_chid.ac3\n");
        printf("  test_eac3_51.ec3\n");
        printf("  test_eac3_71_chid.ec3\n");
        printf("  test_dts_51_chid.dts\n");
        printf("  test_truehd_51_chid.thd\n");
        return 1;
    }
    
    char path[512];
    
    snprintf(path, sizeof(path), "%s/test_ac3_51_chid.ac3", argv[1]);
    test_decoder_format(path, 0, "AC-3 5.1");
    
    snprintf(path, sizeof(path), "%s/test_eac3_51.ec3", argv[1]);
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
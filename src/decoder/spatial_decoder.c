#include "spatial_decoder.h"
#include "spatial_channel_layout.h"
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/system_properties.h>

#define MAX_CHANNELS 8
#define MAX_PACKET_SIZE (4 * 1024 * 1024)  // 4MB for test files

// EAGAIN staging: a parsed packet that avcodec_send_packet() refused
// (decoder holds an undrained frame) must be preserved across calls:
// av_parser_parse2 output aliases parser-internal memory, valid only
// until the next parse call, so it cannot simply be left behind.
// Fixed-size, allocated once with the decoder (never on the audio path).
// 128 KB covers the largest audio access units (MLP max frame: 63768 B).
#define STAGED_PACKET_CAP (128 * 1024)

// PROPERTY_VALUE_MAX is defined in cutils/properties.h but not available in NDK
// Define our own if not available
#ifndef PROPERTY_VALUE_MAX
#define PROPERTY_VALUE_MAX 92
#endif

// Debug property check
static bool is_debug_enabled(void) {
    char value[PROPERTY_VALUE_MAX];
    return __system_property_get("persist.vendor.spatialdm.debug", value) > 0 && atoi(value) > 0;
}

#define DEBUG_PRINT(fmt, ...) do { \
    if (is_debug_enabled()) { \
        printf(fmt, ##__VA_ARGS__); \
        fflush(stdout); \
    } \
} while(0)

struct spatial_decoder {
    AVCodecContext* codec_ctx;
    AVCodecParserContext* parser;
    AVFrame* frame;
    AVPacket* packet;
    spatial_codec_t codec;
    spatial_decoder_config_t config;
    int frame_ready;
    spatial_frame_t current_frame;
    uint8_t packet_buffer[MAX_PACKET_SIZE];
    size_t packet_buffer_size;
    // Unsent parsed packet (see parse_packet stop-and-drain below).
    uint8_t staged_packet[STAGED_PACKET_CAP];
    size_t staged_size;
    int64_t staged_pts;
};

static int codec_to_ffmpeg(spatial_codec_t codec) {
    switch (codec) {
        case 0: return AV_CODEC_ID_AC3;
        case 1: return AV_CODEC_ID_EAC3;
        case 2: return AV_CODEC_ID_DTS;
        case 3: return AV_CODEC_ID_TRUEHD;
        default: return AV_CODEC_ID_NONE;
    }
}

static int parser_to_ffmpeg(spatial_codec_t codec) {
    switch (codec) {
        case 0: return AV_CODEC_ID_AC3;
        case 1: return AV_CODEC_ID_AC3;
        case 2: return AV_CODEC_ID_DTS;
        case 3: return AV_CODEC_ID_MLP;
        default: return AV_CODEC_ID_NONE;
    }
}

static spatial_sample_fmt_t ffmpeg_to_spatial_fmt(enum AVSampleFormat fmt) {
    switch (fmt) {
        case AV_SAMPLE_FMT_S16: return 0;
        case AV_SAMPLE_FMT_S32: return 1;
        case AV_SAMPLE_FMT_FLT: return 2;
        case AV_SAMPLE_FMT_DBL: return 3;
        case AV_SAMPLE_FMT_S16P: return 4;
        case AV_SAMPLE_FMT_S32P: return 5;
        case AV_SAMPLE_FMT_FLTP: return 6;
        case AV_SAMPLE_FMT_DBLP: return 7;
        default: return 0;
    }
}

static int spatial_to_ffmpeg_layout(spatial_layout_t layout, AVChannelLayout* ch_layout) {
    av_channel_layout_uninit(ch_layout);
    
    switch (layout) {
        case 0: // STEREO
            return av_channel_layout_from_mask(ch_layout, AV_CH_LAYOUT_STEREO);
        case 1: // 5.1
            return av_channel_layout_from_mask(ch_layout, AV_CH_LAYOUT_5POINT1);
        case 2: // 7.1
            return av_channel_layout_from_mask(ch_layout, AV_CH_LAYOUT_7POINT1);
        default:
            return av_channel_layout_from_mask(ch_layout, AV_CH_LAYOUT_STEREO);
    }
}

static void build_channel_map(const AVChannelLayout* ch_layout, uint8_t* channel_map, uint8_t* num_channels) {
    DEBUG_PRINT("[build_channel_map] Entry\n");
    
    if (!ch_layout) {
        DEBUG_PRINT("[build_channel_map] ch_layout is NULL!\n");
        return;
    }
    DEBUG_PRINT("[build_channel_map] ch_layout OK\n");
    
    if (!channel_map) {
        DEBUG_PRINT("[build_channel_map] channel_map is NULL!\n");
        return;
    }
    DEBUG_PRINT("[build_channel_map] channel_map OK\n");
    
    if (!num_channels) {
        DEBUG_PRINT("[build_channel_map] num_channels is NULL!\n");
        return;
    }
    DEBUG_PRINT("[build_channel_map] num_channels OK\n");
    
    // Use standard layout based on channel count
    int nch = ch_layout->nb_channels;
    if (nch > 8) nch = 8;
    *num_channels = nch;
    
    // Standard layouts
    if (nch == 6) {
        // 5.1: FL, FR, FC, LFE, SL, SR
        channel_map[0] = 0; channel_map[1] = 1; channel_map[2] = 2;
        channel_map[3] = 3; channel_map[4] = 4; channel_map[5] = 5;
    } else if (nch == 8) {
        // 7.1: FL, FR, FC, LFE, SL, SR, BL, BR
        channel_map[0] = 0; channel_map[1] = 1; channel_map[2] = 2;
        channel_map[3] = 3; channel_map[4] = 4; channel_map[5] = 5;
        channel_map[6] = 6; channel_map[7] = 7;
    } else if (nch == 2) {
        channel_map[0] = 0; channel_map[1] = 1;
    } else {
        for (int i = 0; i < nch; i++) channel_map[i] = i;
    }
    
    *num_channels = nch;
    DEBUG_PRINT("[build_channel_map] Done (nch=%d)\n", nch);
}

spatial_decoder_t* spatial_decoder_create(const spatial_decoder_config_t* config) {
    if (!config) return NULL;
    
    spatial_decoder_t* decoder = calloc(1, sizeof(spatial_decoder_t));
    if (!decoder) return NULL;
    
    decoder->config = *config;
    decoder->codec = config->codec;
    
    decoder->codec_ctx = avcodec_alloc_context3(NULL);
    if (!decoder->codec_ctx) {
        free(decoder);
        return NULL;
    }
    
    decoder->parser = av_parser_init(parser_to_ffmpeg(config->codec));
    if (!decoder->parser) {
        avcodec_free_context(&decoder->codec_ctx);
        free(decoder);
        return NULL;
    }
    
    decoder->frame = av_frame_alloc();
    if (!decoder->frame) {
        av_parser_close(decoder->parser);
        avcodec_free_context(&decoder->codec_ctx);
        free(decoder);
        return NULL;
    }
    
    decoder->packet = av_packet_alloc();
    if (!decoder->packet) {
        av_frame_free(&decoder->frame);
        av_parser_close(decoder->parser);
        avcodec_free_context(&decoder->codec_ctx);
        free(decoder);
        return NULL;
    }
    
    return decoder;
}

void spatial_decoder_destroy(spatial_decoder_t* decoder) {
    if (!decoder) return;
    
    if (decoder->codec_ctx) {
        avcodec_free_context(&decoder->codec_ctx);
    }
    if (decoder->parser) {
        av_parser_close(decoder->parser);
    }
    if (decoder->frame) {
        av_frame_free(&decoder->frame);
    }
    if (decoder->packet) {
        av_packet_free(&decoder->packet);
    }
    free(decoder);
}

int spatial_decoder_open(spatial_decoder_t* decoder) {
    if (!decoder) return -1;
    
    const AVCodec* codec = avcodec_find_decoder(codec_to_ffmpeg(decoder->codec));
    if (!codec) {
        return -3;
    }
    
    decoder->codec_ctx->codec_id = codec_to_ffmpeg(decoder->codec);
    decoder->codec_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
    decoder->codec_ctx->sample_rate = decoder->config.sample_rate;
    decoder->codec_ctx->ch_layout.nb_channels = decoder->config.num_channels;
    
    AVChannelLayout ch_layout;
    if (spatial_to_ffmpeg_layout(decoder->config.layout, &ch_layout) < 0) {
        return -4;
    }
    av_channel_layout_copy(&decoder->codec_ctx->ch_layout, &ch_layout);
    
    int ret = avcodec_open2(decoder->codec_ctx, codec, NULL);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        DEBUG_PRINT("[spatial_decoder] avcodec_open2 failed: %s\n", errbuf);
        return -4;
    }
    
    decoder->frame_ready = 0;
    return 0;
}

int spatial_decoder_close(spatial_decoder_t* decoder) {
    if (!decoder) return -1;
    avcodec_flush_buffers(decoder->codec_ctx);
    decoder->frame_ready = 0;
    return 0;
}

static int parse_packet(spatial_decoder_t* decoder, const uint8_t* data, size_t size, int64_t pts) {
    int consumed = 0;

    while (consumed < (int)size) {
        int used = av_parser_parse2(
            decoder->parser,
            decoder->codec_ctx,
            &decoder->packet->data,
            &decoder->packet->size,
            data + consumed,
            size - consumed,
            pts,
            AV_NOPTS_VALUE,
            0
        );

        if (used < 0) {
            DEBUG_PRINT("[spatial_decoder] parser error: %d\n", used);
            return -5;
        }

        if (used == 0) {
            // Parser needs more data but we've consumed all available input
            // This can happen if the remaining data is less than a complete frame
            break;
        }

        consumed += used;

        if (decoder->packet->size > 0) {
            int ret = avcodec_send_packet(decoder->codec_ctx, decoder->packet);
            if (ret == AVERROR(EAGAIN)) {
                // Stop-and-drain: the decoder still holds an undrained
                // frame, so it refuses new input. Stage this parsed
                // packet (copy: parser output aliases parser-internal
                // memory, invalid after the next parse call) and report
                // progress so far WITHOUT discarding anything. The caller
                // drains via spatial_decoder_receive_frame() and the
                // staged packet is sent first on a later send_packet()
                // call, preserving order.
                if (decoder->packet->size > (int)sizeof(decoder->staged_packet)) {
                    DEBUG_PRINT("[spatial_decoder] staged packet too large: %d\n",
                                decoder->packet->size);
                    return -5;
                }
                memcpy(decoder->staged_packet, decoder->packet->data,
                       decoder->packet->size);
                decoder->staged_size = decoder->packet->size;
                decoder->staged_pts = pts;
                break;
            } else if (ret < 0) {
                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                DEBUG_PRINT("[spatial_decoder] avcodec_send_packet failed: %s\n", errbuf);
                return -5;
            }
            // Successfully sent; keep parsing.
        }
    }

    return consumed;  // Return number of bytes consumed
}

int spatial_decoder_send_packet(spatial_decoder_t* decoder, const uint8_t* data, size_t size, int64_t pts) {
    if (!decoder) return -1;
    /* NOTE: staged retry precedes the data/size guard on purpose so a
       sizeless retry call (size 0) can still flush a tail-staged packet
       when the stream is dry (e.g. at EOS with mStreamValid == 0). */

    // A staged packet from an earlier EAGAIN stop takes precedence to
    // preserve decode order. No new input is consumed until it is sent.
    if (decoder->staged_size > 0) {
        decoder->packet->data = decoder->staged_packet;
        decoder->packet->size = (int)decoder->staged_size;
        decoder->packet->pts = decoder->staged_pts;
        int ret = avcodec_send_packet(decoder->codec_ctx, decoder->packet);
        if (ret == AVERROR(EAGAIN)) {
            return 0;  // Still blocked; caller drains and retries.
        } else if (ret < 0) {
            decoder->staged_size = 0;
            return -5;
        }
        decoder->staged_size = 0;
        // Fall through to parse the new input below.
    }

    if (!data || size == 0) return -1;

    if (size > MAX_PACKET_SIZE) {
        return -5;
    }

    memcpy(decoder->packet_buffer, data, size);
    decoder->packet_buffer_size = size;

    return parse_packet(decoder, decoder->packet_buffer, decoder->packet_buffer_size, pts);
}

int spatial_decoder_receive_frame(spatial_decoder_t* decoder, spatial_frame_t* frame) {
    if (!decoder || !frame) return -1;

    // Single property evaluation per frame (not per log site): the flag
    // is read once here; printing below adds no further property I/O.
    // When disabled (production), this path performs zero logging I/O.
    bool frame_dbg = is_debug_enabled();
    if (frame_dbg) {
        printf("[spatial_decoder] Calling avcodec_receive_frame...\n");
        fflush(stdout);
    }
    int ret = avcodec_receive_frame(decoder->codec_ctx, decoder->frame);
    if (frame_dbg) {
        printf("[spatial_decoder] avcodec_receive_frame returned %d\n", ret);
        fflush(stdout);
    }
    if (ret == AVERROR(EAGAIN)) {
        return -11;
    } else if (ret == AVERROR_EOF) {
        return -7;
    } else if (ret < 0) {
        return -6;
    }
    
    if (frame_dbg) {
        printf("[spatial_decoder] Frame received, nb_samples=%d\n", decoder->frame->nb_samples);
        fflush(stdout);
    }
    frame->nb_samples = decoder->frame->nb_samples;
    frame->sample_rate = decoder->frame->sample_rate;
    frame->format = ffmpeg_to_spatial_fmt(decoder->frame->format);
    frame->pts = decoder->frame->pts;
    frame->duration = decoder->frame->duration;
    
    AVChannelLayout ch_layout;
    av_channel_layout_copy(&ch_layout, &decoder->frame->ch_layout);
    if (frame_dbg) {
        printf("[spatial_decoder] Calling build_channel_map...\n");
        fflush(stdout);
    }
    build_channel_map(&ch_layout, frame->channel_map, &frame->num_channels);
    av_channel_layout_uninit(&ch_layout);

    if (frame_dbg) {
        printf("[spatial_decoder] num_channels=%d\n", frame->num_channels);
        fflush(stdout);
    }
    
    // Bounds check
    if (frame->num_channels > 8) {
        frame->num_channels = 8;
    }
    
    for (int i = 0; i < frame->num_channels; i++) {
        if (decoder->frame->data[i]) {
            frame->data[i] = decoder->frame->data[i];
            frame->linesize[i] = decoder->frame->linesize[i];
        } else {
            frame->data[i] = NULL;
            frame->linesize[i] = 0;
        }
    }
    
    if (frame_dbg) {
        printf("[spatial_decoder] receive_frame done\n");
        fflush(stdout);
    }
    return 0;
}

int spatial_decoder_flush(spatial_decoder_t* decoder) {
    if (!decoder) return -1;
    avcodec_flush_buffers(decoder->codec_ctx);
    decoder->frame_ready = 0;
    // A flush establishes a fresh sync point: pre-flush bytes must never
    // be decoded afterwards, so drop any staged unsent packet as well.
    decoder->staged_size = 0;
    decoder->staged_pts = 0;
    return 0;
}

const char* spatial_decoder_strerror(int err) {
    switch (err) {
        case 0: return "OK";
        case -1: return "Error";
        case -2: return "Invalid argument";
        case -3: return "Decoder not found";
        case -4: return "Open failed";
        case -5: return "Send packet failed";
        case -6: return "Receive frame failed";
        case -7: return "EOF";
        case -11: return "Try again";
        default: return "Unknown error";
    }
}

const char* spatial_decoder_get_codec_name(spatial_codec_t codec) {
    switch (codec) {
        case 0: return "AC-3";
        case 1: return "E-AC-3";
        case 2: return "DTS";
        case 3: return "TrueHD";
        default: return "UNKNOWN";
    }
}
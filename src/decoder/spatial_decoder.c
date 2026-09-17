#include "spatial_decoder.h"
#include "spatial_channel_layout.h"
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_CHANNELS 8
#define MAX_PACKET_SIZE (128 * 1024)

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
};

static int codec_to_ffmpeg(spatial_codec_t codec) {
    switch (codec) {
        case 0: return AV_CODEC_ID_AC3;
        case 1: return AV_CODEC_ID_EAC3;
        case 2: return AV_CODEC_ID_DCA;
        case 3: return AV_CODEC_ID_TRUEHD;
        default: return AV_CODEC_ID_NONE;
    }
}

static int parser_to_ffmpeg(spatial_codec_t codec) {
    switch (codec) {
        case 0: return AV_CODEC_ID_AC3;
        case 1: return AV_CODEC_ID_AC3;
        case 2: return AV_CODEC_ID_DCA;
        case 3: return AV_CODEC_ID_MLP;
        default: return AV_CODEC_ID_NONE;
    }
}

static spatial_sample_fmt_t ffmpeg_to_spatial_fmt(AVSampleFormat fmt) {
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

static void build_channel_map(AVChannelLayout* ch_layout, uint8_t* channel_map, int* num_channels) {
    *num_channels = ch_layout->nb_channels;
    for (int i = 0; i < ch_layout->nb_channels && i < 8; i++) {
        enum AVChannel ch = ch_layout->u.mask[i];
        switch (ch) {
            case AV_CHAN_FRONT_LEFT: channel_map[i] = 0; break;
            case AV_CHAN_FRONT_RIGHT: channel_map[i] = 1; break;
            case AV_CHAN_FRONT_CENTER: channel_map[i] = 2; break;
            case AV_CHAN_LOW_FREQUENCY: channel_map[i] = 3; break;
            case AV_CHAN_SIDE_LEFT: channel_map[i] = 4; break;
            case AV_CHAN_SIDE_RIGHT: channel_map[i] = 5; break;
            case AV_CHAN_BACK_LEFT: channel_map[i] = 6; break;
            case AV_CHAN_BACK_RIGHT: channel_map[i] = 7; break;
            default: channel_map[i] = 255; break;
        }
    }
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
        printf("[spatial_decoder] avcodec_open2 failed: %s\n", errbuf);
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
    int ret = 0;
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
            return -5;
        }
        
        consumed += used;
        
        if (decoder->packet->size > 0) {
            ret = avcodec_send_packet(decoder->codec_ctx, decoder->packet);
            if (ret < 0) {
                return -5;
            }
        }
    }
    
    return 0;
}

int spatial_decoder_send_packet(spatial_decoder_t* decoder, const uint8_t* data, size_t size, int64_t pts) {
    if (!decoder || !data || size == 0) return -1;
    
    if (size > MAX_PACKET_SIZE) {
        return -5;
    }
    
    memcpy(decoder->packet_buffer, data, size);
    decoder->packet_buffer_size = size;
    
    return parse_packet(decoder, decoder->packet_buffer, decoder->packet_buffer_size, pts);
}

int spatial_decoder_receive_frame(spatial_decoder_t* decoder, spatial_frame_t* frame) {
    if (!decoder || !frame) return -1;
    
    int ret = avcodec_receive_frame(decoder->codec_ctx, decoder->frame);
    if (ret == AVERROR(EAGAIN)) {
        return -11;
    } else if (ret == AVERROR_EOF) {
        return -7;
    } else if (ret < 0) {
        return -6;
    }
    
    frame->nb_samples = decoder->frame->nb_samples;
    frame->sample_rate = decoder->frame->sample_rate;
    frame->format = ffmpeg_to_spatial_fmt(decoder->frame->format);
    frame->pts = decoder->frame->pts;
    frame->duration = decoder->frame->pkt_duration;
    
    AVChannelLayout ch_layout;
    av_channel_layout_copy(&ch_layout, &decoder->frame->ch_layout);
    build_channel_map(&ch_layout, frame->channel_map, &frame->num_channels);
    av_channel_layout_uninit(&ch_layout);
    
    for (int i = 0; i < frame->num_channels; i++) {
        frame->data[i] = decoder->frame->data[i];
        frame->linesize[i] = decoder->frame->linesize[i];
    }
    
    return 0;
}

int spatial_decoder_flush(spatial_decoder_t* decoder) {
    if (!decoder) return -1;
    avcodec_flush_buffers(decoder->codec_ctx);
    decoder->frame_ready = 0;
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
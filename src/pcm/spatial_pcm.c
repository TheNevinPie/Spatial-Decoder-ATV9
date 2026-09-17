#include "spatial_pcm.h"
#include "spatial_channel_layout.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

struct spatial_pcm_converter {
    spatial_pcm_format_t input_format;
    spatial_pcm_format_t output_format;
    int remap_indices[8];
    uint8_t* temp_buffer;
    size_t temp_buffer_size;
    int needs_resample;
    double resample_ratio;
    int resample_filter_len;
    double* resample_filter;
    int resample_pos;
    double* resample_input_buffer;
    int resample_input_size;
    int resample_input_pos;
};

static int get_bytes_per_sample(spatial_sample_fmt_t fmt) {
    switch (fmt) {
        case 0: return 2;   // S16
        case 1: return 4;   // S32
        case 2: return 4;   // FLT
        case 3: return 8;   // DBL
        case 4: return 2;   // S16P
        case 5: return 4;   // S32P
        case 6: return 4;   // FLTP
        case 7: return 8;   // DBLP
        default: return 0;
    }
}

static bool is_planar(spatial_sample_fmt_t fmt) {
    return fmt >= 4;
}

int spatial_pcm_format_get_bytes_per_sample(spatial_sample_fmt_t fmt) {
    return get_bytes_per_sample(fmt);
}

int spatial_pcm_format_is_planar(spatial_sample_fmt_t fmt) {
    return is_planar(fmt) ? 1 : 0;
}

const char* spatial_sample_fmt_name(spatial_sample_fmt_t fmt) {
    static const char* names[] = {
        "S16", "S32", "FLT", "DBL", "S16P", "S32P", "FLTP", "DBLP"
    };
    if (fmt < 8) return names[fmt];
    return "UNKNOWN";
}

spatial_layout_t spatial_layout_from_channel_count(int channels) {
    switch (channels) {
        case 2: return 0;
        case 6: return 1;
        case 8: return 2;
        default: return 0;
    }
}

int spatial_layout_get_channel_count(spatial_layout_t layout) {
    switch (layout) {
        case 0: return 2;
        case 1: return 6;
        case 2: return 8;
        default: return 0;
    }
}

const char* spatial_channel_name(int channel) {
    static const char* names[] = {"FL", "FR", "FC", "LFE", "SL", "SR", "BL", "BR"};
    if (channel >= 0 && channel < 8) return names[channel];
    return "UNK";
}

const char* spatial_layout_name(int layout) {
    static const char* names[] = {"STEREO", "5.1", "7.1"};
    if (layout >= 0 && layout < 3) return names[layout];
    return "UNKNOWN";
}

static void build_sinc_filter(double* filter, int len, double cutoff) {
    for (int i = 0; i < len; i++) {
        double x = (i - (len - 1) / 2.0) * cutoff;
        if (x == 0) {
            filter[i] = 1.0;
        } else {
            filter[i] = sin(x) / x;
        }
        double window = 0.54 - 0.46 * cos(2 * M_PI * i / (len - 1));
        filter[i] *= window;
    }
    
    double sum = 0;
    for (int i = 0; i < len; i++) sum += filter[i];
    for (int i = 0; i < len; i++) filter[i] /= sum;
}

spatial_pcm_converter_t* spatial_pcm_converter_create(
    const spatial_pcm_format_t* input_format,
    const spatial_pcm_format_t* output_format) {
    
    if (!input_format || !output_format) return NULL;
    
    spatial_pcm_converter_t* conv = calloc(1, sizeof(spatial_pcm_converter_t));
    if (!conv) return NULL;
    
    conv->input_format = *input_format;
    conv->output_format = *output_format;
    
    spatial_channel_map_t src_map, dst_map;
    spatial_channel_map_create(input_format->layout, &src_map);
    spatial_channel_map_create(output_format->layout, &dst_map);
    
    int remap[8];
    spatial_channel_map_remap(&src_map, &dst_map, remap);
    for (int i = 0; i < 8; i++) {
        conv->remap_indices[i] = remap[i];
    }
    
    if (input_format->sample_rate != output_format->sample_rate) {
        conv->needs_resample = 1;
        conv->resample_ratio = (double)output_format->sample_rate / input_format->sample_rate;
        conv->resample_filter_len = 64;
        conv->resample_filter = malloc(conv->resample_filter_len * sizeof(double));
        build_sinc_filter(conv->resample_filter, conv->resample_filter_len, 0.95 / conv->resample_ratio);
        
        conv->resample_input_size = (int)(4096 * conv->resample_ratio) + 16;
        conv->resample_input_buffer = malloc(conv->resample_input_size * 8 * sizeof(double));
    }
    
    return conv;
}

void spatial_pcm_converter_destroy(spatial_pcm_converter_t* converter) {
    if (!converter) return;
    free(converter->temp_buffer);
    free(converter->resample_filter);
    free(converter->resample_input_buffer);
    free(converter);
}

static int convert_sample_format(const void* input, void* output, int samples, spatial_sample_fmt_t in_fmt, spatial_sample_fmt_t out_fmt) {
    if (in_fmt == out_fmt) {
        int bytes = get_bytes_per_sample(in_fmt);
        memcpy(output, input, samples * bytes);
        return 0;
    }
    
    for (int i = 0; i < samples; i++) {
        double sample = 0;
        
        switch (in_fmt) {
            case 0: sample = ((int16_t*)input)[i] / 32768.0; break;
            case 1: sample = ((int32_t*)input)[i] / 2147483648.0; break;
            case 2: sample = ((float*)input)[i]; break;
            case 3: sample = ((double*)input)[i]; break;
            case 4: sample = ((int16_t*)input)[i] / 32768.0; break;
            case 5: sample = ((int32_t*)input)[i] / 2147483648.0; break;
            case 6: sample = ((float*)input)[i]; break;
            case 7: sample = ((double*)input)[i]; break;
        }
        
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        
        switch (out_fmt) {
            case 0: ((int16_t*)output)[i] = (int16_t)(sample * 32767.0); break;
            case 1: ((int32_t*)output)[i] = (int32_t)(sample * 2147483647.0); break;
            case 2: ((float*)output)[i] = (float)sample; break;
            case 3: ((double*)output)[i] = sample; break;
            case 4: ((int16_t*)output)[i] = (int16_t)(sample * 32767.0); break;
            case 5: ((int32_t*)output)[i] = (int32_t)(sample * 2147483647.0); break;
            case 6: ((float*)output)[i] = (float)sample; break;
            case 7: ((double*)output)[i] = sample; break;
        }
    }
    return 0;
}

int spatial_pcm_converter_process(spatial_pcm_converter_t* conv,
                                  const uint8_t* const input_data[8],
                                  uint8_t* const output_data[8],
                                  int nb_samples) {
    if (!conv || !input_data || !output_data || nb_samples <= 0) return -1;
    
    int in_bytes = get_bytes_per_sample(conv->input_format.format);
    int out_bytes = get_bytes_per_sample(conv->output_format.format);
    bool in_planar = is_planar(conv->input_format.format);
    bool out_planar = is_planar(conv->output_format.format);
    
    int in_channels = conv->input_format.num_channels;
    int out_channels = conv->output_format.num_channels;
    
    if (conv->needs_resample) {
        int out_samples = (int)(nb_samples * conv->resample_ratio) + 1;
        
        for (int ch = 0; ch < out_channels; ch++) {
            int src_ch = conv->remap_indices[ch];
            if (src_ch < 0 || src_ch >= in_channels) {
                memset(output_data[ch], 0, out_samples * out_bytes);
                continue;
            }
            
            double* src = (double*)conv->temp_buffer;
            if (!src) {
                src = malloc(nb_samples * sizeof(double));
                free(conv->temp_buffer);
                conv->temp_buffer = src;
            }
            
            for (int i = 0; i < nb_samples; i++) {
                if (in_planar) {
                    const uint8_t* ch_data = input_data[src_ch] + i * in_bytes;
                } else {
                    const uint8_t* ch_data = input_data[0] + (i * in_channels + src_ch) * in_bytes;
                }
            }
            
            for (int i = 0; i < out_samples; i++) {
                double sum = 0;
                for (int j = 0; j < conv->resample_filter_len; j++) {
                    int src_idx = (int)(i / conv->resample_ratio) - conv->resample_filter_len / 2 + j;
                    if (src_idx >= 0 && src_idx < nb_samples) {
                        sum += src[src_idx] * conv->resample_filter[j];
                    }
                }
                
                if (out_planar) {
                    switch (conv->output_format.format) {
                        case 0: ((int16_t*)output_data[ch])[i] = (int16_t)(sum * 32767); break;
                        case 2: ((float*)output_data[ch])[i] = (float)sum; break;
                    }
                } else {
                    switch (conv->output_format.format) {
                        case 0: ((int16_t*)output_data[0])[i * out_channels + ch] = (int16_t)(sum * 32767); break;
                        case 2: ((float*)output_data[0])[i * out_channels + ch] = (float)sum; break;
                    }
                }
            }
        }
    } else {
        for (int ch = 0; ch < out_channels; ch++) {
            int src_ch = conv->remap_indices[ch];
            if (src_ch < 0 || src_ch >= in_channels) {
                memset(output_data[ch], 0, nb_samples * out_bytes);
                continue;
            }
            
            if (in_planar && out_planar) {
                convert_sample_format(input_data[src_ch], output_data[ch], nb_samples,
                                    conv->input_format.format, conv->output_format.format);
            } else if (!in_planar && !out_planar) {
                for (int i = 0; i < nb_samples; i++) {
                    const uint8_t* src = input_data[0] + (i * in_channels + src_ch) * in_bytes;
                    uint8_t* dst = output_data[0] + (i * out_channels + ch) * out_bytes;
                    convert_sample_format(src, dst, 1, conv->input_format.format, conv->output_format.format);
                }
            } else if (in_planar && !out_planar) {
                for (int i = 0; i < nb_samples; i++) {
                    const uint8_t* src = input_data[src_ch] + i * in_bytes;
                    uint8_t* dst = output_data[0] + (i * out_channels + ch) * out_bytes;
                    convert_sample_format(src, dst, 1, conv->input_format.format, conv->output_format.format);
                }
            } else {
                for (int i = 0; i < nb_samples; i++) {
                    const uint8_t* src = input_data[0] + (i * in_channels + src_ch) * in_bytes;
                    uint8_t* dst = output_data[ch] + i * out_bytes;
                    convert_sample_format(src, dst, 1, conv->input_format.format, conv->output_format.format);
                }
            }
        }
    }
    
    return 0;
}

int spatial_pcm_converter_flush(spatial_pcm_converter_t* converter) {
    if (!converter) return -1;
    converter->resample_pos = 0;
    converter->resample_input_pos = 0;
    return 0;
}
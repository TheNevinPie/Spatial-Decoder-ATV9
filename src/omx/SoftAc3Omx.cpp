#define LOG_TAG "SoftAc3Omx"
#include "spatial_android_compat.h"

#include "SoftAc3Omx.h"

#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/hexdump.h>

#include <media/stagefright/omx/OMXComponent.h>

#include <media/MediaCodecList.h>
#include <media/MediaCodecInfo.h>
#include <media/MediaCodec.h>

namespace android {

// Helper function to map MIME type to spatial_codec_t
static spatial_codec_t mimeToSpatialCodec(const char* mime) {
    if (!mime) return SPATIAL_CODEC_AC3;
    
    if (strcmp(mime, "audio/ac3") == 0) return SPATIAL_CODEC_AC3;
    if (strcmp(mime, "audio/eac3") == 0) return SPATIAL_CODEC_EAC3;
    if (strcmp(mime, "audio/dts") == 0) return SPATIAL_CODEC_DTS;
    if (strcmp(mime, "audio/vnd.dolby.mlp") == 0) return SPATIAL_CODEC_TRUEHD;
    if (strcmp(mime, "audio/truehd") == 0) return SPATIAL_CODEC_TRUEHD;
    
    return SPATIAL_CODEC_AC3; // Default fallback
}

namespace android {

// Helper function to check debug property
static bool isDebugEnabled() {
    char value[PROPERTY_VALUE_MAX];
    return __system_property_get("persist.vendor.spatialdm.debug", value) > 0 && atoi(value) > 0;
}

// Helper function to log downmix config matrix
static void logDownmixConfig(const char* tag, const spatial_downmix_config_t* config) {
    if (!isDebugEnabled()) return;
    
    if (config->layout == SPATIAL_LAYOUT_5_1) {
        ALOGI("%s: 5.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f S=%.3f LFE=%.3f", tag,
              config->gains_5_1.left, config->gains_5_1.right,
              config->gains_5_1.center, config->gains_5_1.surround,
              config->gains_5_1.lfe);
    } else if (config->layout == SPATIAL_LAYOUT_7_1) {
        ALOGI("%s: 7.1 downmix matrix:", tag);
        ALOGI("%s:   FL=%.3f FR=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f", tag,
              config->gains_7_1.left, config->gains_7_1.right,
              config->gains_7_1.center, config->gains_7_1.side,
              config->gains_7_1.rear, config->gains_7_1.lfe);
    }
}

namespace android {

// Helper function to map MIME to spatial_codec_t
static spatial_codec_t mimeToSpatialCodec(const char* mime) {
    if (!mime) return SPATIAL_CODEC_AC3;
    
    if (strcmp(mime, "audio/ac3") == 0) return SPATIAL_CODEC_AC3;
    if (strcmp(mime, "audio/eac3") == 0) return SPATIAL_CODEC_EAC3;
    if (strcmp(mime, "audio/dts") == 0) return SPATIAL_CODEC_DTS;
    if (strcmp(mime, "audio/vnd.dolby.mlp") == 0) return SPATIAL_CODEC_TRUEHD;
    if (strcmp(mime, "audio/truehd") == 0) return SPATIAL_CODEC_TRUEHD;
    
    return SPATIAL_CODEC_AC3;
}

SoftAc3Omx::SoftAc3Omx(const char* name, const OMX_CALLBACKTYPE* callbacks,
                       OMX_PTR appData, OMX_COMPONENTTYPE** component)
    : SimpleOMXComponent(name, callbacks, appData, component),
      mState(kStateIdle),
      mStreamBuffer(NULL),
      mStreamBufferValid(0),
      mStreamBufferCapacity(32 * 1024),
      mDecoder(NULL),
      mDownmix(NULL),
      mPcmConverter(NULL),
      mFramesDecoded(0),
      mTotalSamples(0),
      mSawEOS(false),
      mSentEOS(false) {
    memset(&mFrameOut, 0, sizeof(mFrameOut));
    memset(mPlanarPtrs, 0, sizeof(mPlanarPtrs));
    memset(mOutBuffers, 0, sizeof(mOutBuffers));
    memset(mOutPtrs, 0, sizeof(mOutPtrs));

    mStreamBuffer = new uint8_t[32 * 1024];
    mStreamBufferCapacity = 32 * 1024;
    mStreamBufferValid = 0;

    initDecoder();
}

SoftAc3Omx::~SoftAc3Omx() {
    releaseDecoder();
    delete[] mStreamBuffer;
}

status_t SoftAc3Omx::initDecoder() {
    // Get MIME type from component role or default to AC3
    const char* mime = getMimeTypeFromRole();
    spatial_codec_t codec = mimeToSpatialCodec(mime);
    
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

    mDecoder = spatial_decoder_create(&config);
    if (!mDecoder) {
        ALOGE("Failed to create spatial decoder for codec %s", spatial_decoder_get_codec_name(codec));
        return UNKNOWN_ERROR;
    }

    int ret = spatial_decoder_open(mDecoder);
    if (ret != 0) {
        ALOGE("Failed to open spatial decoder: %s", spatial_decoder_strerror(ret));
        spatial_decoder_destroy(mDecoder);
        mDecoder = NULL;
        return UNKNOWN_ERROR;
    }

    // Log decoder initialization
    ALOGI("Decoder created: %s, layout: %s, sample_rate: %d, channels: %d",
          spatial_decoder_get_codec_name(codec),
          spatial_layout_name(config.layout),
          config.sample_rate, config.num_channels);

    // Initialize downmix with defaults (will be overridden by property bridge if needed)
    spatial_downmix_get_default_config_5_1(&mDownmixConfig);
    
    // Only set default gains if not already set by property bridge
    // The property bridge will update these via spatial_config_update_from_property
    mDownmixConfig.matrix_oba = SPATIAL_MATRIX_NONE;
    mDownmixConfig.matrix_cba = SPATIAL_MATRIX_NONE;
    mDownmixConfig.content_type = SPATIAL_CONTENT_CBA;
    mDownmixConfig.debug_enabled = isDebugEnabled();

    mDownmix = spatial_downmix_create(&mDownmixConfig);
    if (!mDownmix) {
        ALOGE("Failed to create downmix");
        return UNKNOWN_ERROR;
    }

    // Log downmix config if debug enabled
    if (isDebugEnabled()) {
        logDownmixConfig("SoftAc3Omx::initDecoder", &mDownmixConfig);
    }

    // PCM converter setup (decoder output -> float planar stereo)
    spatial_pcm_format_t dec_format = {0};
    dec_format.format = SPATIAL_SAMPLE_FMT_FLTP;
    dec_format.sample_rate = 48000;
    dec_format.layout = SPATIAL_LAYOUT_5_1;
    dec_format.num_channels = 6;
    dec_format.channel_map[0] = 0;
    dec_format.channel_map[1] = 1;
    dec_format.channel_map[2] = 2;
    dec_format.channel_map[3] = 3;
    dec_format.channel_map[4] = 4;
    dec_format.channel_map[5] = 5;

    spatial_pcm_format_t out_format = {0};
    out_format.format = SPATIAL_SAMPLE_FMT_FLTP;
    out_format.sample_rate = 48000;
    out_format.layout = SPATIAL_LAYOUT_STEREO;
    out_format.num_channels = 2;
    out_format.channel_map[0] = 0;
    out_format.channel_map[1] = 1;

    mPcmConverter = spatial_pcm_converter_create(&dec_format, &out_format);
    if (!mPcmConverter) {
        ALOGE("Failed to create PCM converter");
        return UNKNOWN_ERROR;
    }

    // Downmix
    mDownmixConfig.layout = SPATIAL_LAYOUT_5_1;
    mDownmixConfig.num_input_channels = 6;
    mDownmix = spatial_downmix_create(&mDownmixConfig);
    if (!mDownmix) {
        ALOGE("Failed to create downmix");
        return UNKNOWN_ERROR;
    }

    // Log decoder creation
    ALOGI("Decoder initialized: codec=%s, layout=%s, sr=%d, ch=%d",
          spatial_decoder_get_codec_name(mDownmixConfig.layout == SPATIAL_LAYOUT_7_1 ? SPATIAL_CODEC_EAC3 : SPATIAL_CODEC_AC3),
          spatial_layout_name(SPATIAL_LAYOUT_5_1), 48000, 6);

    return OK;
}

void SoftAc3Omx::releaseDecoder() {
    if (mPcmConverter) {
        spatial_pcm_converter_destroy(mPcmConverter);
        mPcmConverter = NULL;
    }
    if (mDownmix) {
        spatial_downmix_destroy(mDownmix);
        mDownmix = NULL;
    }
    if (mDecoder) {
        spatial_decoder_destroy(mDecoder);
        mDecoder = NULL;
    }
}

void SoftAc3Omx::onReset() {
    mState = kStateIdle;
    mSawEOS = false;
    mSentEOS = false;
    mStreamBufferValid = 0;
    mFramesDecoded = 0;
    mTotalSamples = 0;

    if (mDecoder) {
        spatial_decoder_close(mDecoder);
    }
}

OMX_ERRORTYPE SoftAc3Omx::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamAudioAc3:
        case OMX_IndexParamAudioEac3:
        case OMX_IndexParamAudioDts:
        case OMX_IndexParamAudioTrueHd:
        case OMX_IndexParamPortDefinition:
        case OMX_IndexParamAudioPcm:
        case OMX_IndexParamAudioInit:
        case OMX_IndexParamStandardComponentRole:
            return SimpleOMXComponent::internalGetParameter(index, params);
        default:
            return OMX_ErrorUnsupportedIndex;
    }
}

OMX_ERRORTYPE SoftAc3Omx::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamAudioAc3:
        case OMX_IndexParamAudioEac3:
        case OMX_IndexParamAudioDts:
        case OMX_IndexParamAudioTrueHd:
        case OMX_IndexParamPortDefinition:
        case OMX_IndexParamAudioPcm:
            return SimpleOMXComponent::internalSetParameter(index, params);
        default:
            return OMX_ErrorUnsupportedIndex;
    }
}

OMX_ERRORTYPE SoftAc3Omx::getConfig(OMX_INDEXTYPE index, OMX_PTR params) {
    return SimpleOMXComponent::getConfig(index, params);
}

OMX_ERRORTYPE SoftAc3Omx::setConfig(OMX_INDEXTYPE index, const OMX_PTR params) {
    return SimpleOMXComponent::setConfig(index, params);
}

void SoftAc3Omx::onQueueFilled(OMX_U32 portIndex) {
    if (mState != kStateExecuting) {
        return;
    }

    if (portIndex == 0) {  // Input port
        readInputData();
        sendPacketToDecoder();
    } else if (portIndex == 1) {  // Output port
        drainOutputFrames();
    }
}

void SoftAc3Omx::readInputData() {
    List<BufferInfo>& inQueue = getPortQueue(0);
    while (!inQueue.empty()) {
        BufferInfo* info = *inQueue.begin();
        OMX_BUFFERHEADERTYPE* buffer = info->mHeader;

        if (buffer->nFlags & OMX_BUFFERFLAG_EOS) {
            ALOGI("Received EOS on input port");
            mSawEOS = true;
            // Queue the EOS buffer to be sent to decoder
            queueInputBuffer(buffer);
            inQueue.erase(inQueue.begin());
            return;
        }

        if (buffer->nFilledLen > 0) {
            // Add data to stream buffer
            if (mStreamBufferValid + buffer->nFilledLen > mStreamBufferCapacity) {
                ALOGW("Stream buffer overflow, flushing");
                sendPacketToDecoder();
            }

            memcpy(mStreamBuffer + mStreamBufferValid,
                   buffer->pBuffer + buffer->nOffset,
                   buffer->nFilledLen);
            mStreamBufferValid += buffer->nFilledLen;
        }

        inQueue.erase(inQueue.begin());
        buffer->nFilledLen = 0;
        queueEmptyBuffer(buffer);
    }

    // Send data from stream buffer to decoder
    sendPacketToDecoder();
}

void SoftAc3Omx::sendPacketToDecoder() {
    while (mStreamBufferValid > 0) {
        int ret = spatial_decoder_send_packet(mDecoder, mStreamBuffer, mStreamBufferValid, 0);
        if (ret > 0) {
            // Shift remaining data to front
            size_t consumed = send_ret;
            if (mStreamBufferValid > consumed) {
                memmove(mStreamBuffer, mStreamBuffer + send_ret,
                        mStreamBufferValid - send_ret);
            }
            mStreamBufferValid -= send_ret;
        } else if (ret == -11) {  // EAGAIN
            // Drain output frames and retry
            drainOutputFrames();
        } else if (ret < 0) {
            ALOGE("Decoder send failed: %s", spatial_decoder_strerror(ret));
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            return;
        } else {
            // 0 bytes consumed, need more data
            break;
        }
    }

    // Try to drain output frames periodically
    if (mStreamBufferValid == 0 || mFramesDecoded % 10 == 0) {
        drainOutputFrames();
    }
}

void SoftAc3Omx::drainOutputFrames() {
    spatial_frame_t frame_out = {0};
    while (1) {
        int ret = spatial_decoder_receive_frame(mDecoder, &mFrameOut);
        if (ret == -11) {  // EAGAIN
            break;
        } else if (ret == -7) {  // AVERROR_EOF
            // Map AVERROR_EOF to OMX_ErrorNoMore for proper EOS handling
            ALOGI("Decoder reached EOF");
            mSawEOS = true;
            // Try to fill one last output buffer with EOS flag
            if (!getPortQueue(1).empty()) {
                fillOutputBuffer(nullptr, 0);  // Send EOS buffer
            }
            break;
        } else if (ret < 0) {
            ALOGE("Decode error: %s", spatial_decoder_strerror(ret));
            break;
        }

        // Convert to float planar stereo
        float* planar_ptrs[8];
        for (int ch = 0; ch < mFrameOut.num_channels; ch++) {
            mPlanarPtrs[ch] = (float*)mFrameOut.data[ch];
        }

        for (int ch = 0; ch < 2; ch++) {
            mOutPtrs[ch] = mOutBuffers[ch];
        }

        int conv_ret = spatial_pcm_converter_process(mPcmConverter,
                                                     (const uint8_t* const*)mPlanarPtrs,
                                                     (uint8_t**)mOutPtrs,
                                                     mFrameOut.nb_samples);
        if (conv_ret == 0) {
            fillOutputBuffer(mOutPtrs[0], mFrameOut.nb_samples);
        }
    }
}

void SoftAc3Omx::fillOutputBuffer(float* data, int num_samples) {
    List<BufferInfo>& outQueue = getPortQueue(1);
    if (outQueue.empty()) {
        return;
    }

    BufferInfo* info = *outQueue.begin();
    OMX_BUFFERHEADERTYPE* buffer = info->mHeader;

    if (data == nullptr && num_samples == 0) {
        // EOS buffer - send with EOS flag
        buffer->nFilledLen = 0;
        buffer->nFlags = OMX_BUFFERFLAG_EOS;
        buffer->nOffset = 0;
        buffer->nTimeStamp = 0;
        outQueue.erase(outQueue.begin());
        queueEmptyBuffer(buffer);
        mSentEOS = true;
        return;
    }

    size_t bytes_per_sample = 4;  // float32
    size_t total_bytes = 2 * num_samples * sizeof(float);  // stereo

    if (buffer->nAllocLen < total_bytes) {
        ALOGE("Output buffer too small: %zu < %zu", buffer->nAllocLen, total_bytes);
        return;
    }

    // Interleave left/right channels
    float* left = mOutPtrs[0];
    float* right = mOutPtrs[1];
    int16_t* out = (int16_t*)buffer->pBuffer;
    for (int i = 0; i < num_samples; i++) {
        // Convert float to 16-bit PCM with clipping
        float l = left[i];
        float r = right[i];
        if (l > 1.0f) l = 1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f;
        if (r < -1.0f) r = -1.0f;
        out[i * 2] = (int16_t)(l * 32767.0f);
        out[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }

    buffer->nFilledLen = num_samples * 2 * sizeof(int16_t);
    buffer->nFlags = 0;
    buffer->nOffset = 0;
    buffer->nTimeStamp = 0;  // TODO: proper timestamp

    outQueue.erase(outQueue.begin());
    queueEmptyBuffer(buffer);
}

void SoftAc3Omx::onReset() {
    mState = kStateIdle;
    mSawEOS = false;
    mSentEOS = false;
    mStreamBufferValid = 0;
    mFramesDecoded = 0;
    mTotalSamples = 0;

    if (mDecoder) {
        spatial_decoder_close(mDecoder);
    }
}

}  // namespace android
// SpatialOmxAdapter.cpp — OMX compatibility adapter for libspatialdecoder.
//
// See SpatialOmxAdapter.h for the architecture and ABI contract notes.
//
// Pipeline (per decoded frame):
//   spatial_decoder (FFmpeg, native fmt/rate/layout)
//     -> spatial_pcm_converter (multichannel FLTP, resampled to 48 kHz)
//     -> interleave -> spatial_downmix_process (stereo float)
//     -> float->S16 -> output staging -> OMX output buffers (stereo S16
//        interleaved 48 kHz, matching the reference component).
//
// Four libraries share these sources; DEFAULT_CODEC_NAME selects the
// fallback codec for unrecognized component names.

#define LOG_TAG "SpatialOmxAdapter"
#include <utils/Log.h>

#include <cutils/properties.h>

#include "SpatialOmxAdapter.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef DEFAULT_CODEC_NAME
#define DEFAULT_CODEC_NAME "ac3dec"
#endif

namespace android {

// Local InitOMXParams (the framework's template lives outside the
// vendored headers; behavior: zero + nSize + version 1.0.0.0).
template <class T>
static void InitOMXParams(T *params) {
    memset(params, 0, sizeof(T));
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

// ---------------------------------------------------------------------------
// Codec identity. Single authoritative mapping: OMX component name (as
// passed by SoftOMXPlugin) or internal name -> spatial_codec_t.
// ---------------------------------------------------------------------------

spatial_codec_t SpatialOmxAdapter::componentNameToSpatialCodec(const char *name) {
    if (name == NULL) {
        goto fallback;
    }
    if (strcmp(name, "OMX.google.ac3.decoder") == 0
            || strcmp(name, "ac3dec") == 0) {
        return SPATIAL_CODEC_AC3;
    }
    if (strcmp(name, "OMX.google.eac3.decoder") == 0
            || strcmp(name, "eac3dec") == 0) {
        return SPATIAL_CODEC_EAC3;
    }
    if (strcmp(name, "OMX.google.dts.decoder") == 0
            || strcmp(name, "OMX.google.dtshd.decoder") == 0
            || strcmp(name, "OMX.google.dtse.decoder") == 0
            || strcmp(name, "OMX.google.dtslbr.dec") == 0
            || strcmp(name, "dtsdec") == 0) {
        return SPATIAL_CODEC_DTS;
    }
    if (strcmp(name, "OMX.google.truehd.decoder") == 0
            || strcmp(name, "truehddec") == 0) {
        return SPATIAL_CODEC_TRUEHD;
    }
fallback: {
        const char *def = DEFAULT_CODEC_NAME;
        if (strcmp(def, "eac3dec") == 0) {
            return SPATIAL_CODEC_EAC3;
        }
        if (strcmp(def, "dtsdec") == 0) {
            return SPATIAL_CODEC_DTS;
        }
        if (strcmp(def, "truehddec") == 0) {
            return SPATIAL_CODEC_TRUEHD;
        }
        return SPATIAL_CODEC_AC3;
    }
}

const char *SpatialOmxAdapter::roleForCodec(spatial_codec_t codec) {
    switch (codec) {
        case SPATIAL_CODEC_EAC3:
            return "audio_decoder.eac3";
        case SPATIAL_CODEC_DTS:
            return "audio_decoder.dts";
        case SPATIAL_CODEC_TRUEHD:
            return "audio_decoder.truehd";
        case SPATIAL_CODEC_AC3:
        default:
            return "audio_decoder.ac3";
    }
}

// Roles accepted by internalSetParameter (primary + aliases observed in
// the reference binary's role table).
bool SpatialOmxAdapter::isRoleForCodec(const char *role, spatial_codec_t codec) {
    if (role == NULL) {
        return false;
    }
    if (strcmp(role, roleForCodec(codec)) == 0) {
        return true;
    }
    switch (codec) {
        case SPATIAL_CODEC_EAC3:
            return strcmp(role, "audio_decoder.ac3p") == 0;
        case SPATIAL_CODEC_DTS:
            return strcmp(role, "audio_decoder.dtshd") == 0
                    || strcmp(role, "audio_decoder.dtse") == 0;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Construction / destruction.
// ---------------------------------------------------------------------------

SpatialOmxAdapter::SpatialOmxAdapter(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SimpleSoftOMXComponent(name, callbacks, appData, component),
      mCodec(componentNameToSpatialCodec(name)),
      mRole(roleForCodec(mCodec)),
      mInputCoding(OMX_AUDIO_CodingAutoDetect),
      mInitCheck(OMX_ErrorNone),
      mDecoder(NULL),
      mPcm(NULL),
      mDownmix(NULL),
      mPropCtx(NULL),
      mConvInFmt(SPATIAL_SAMPLE_FMT_FLTP),
      mConvInRate(0),
      mConvInCh(0),
      mConvInLayout(SPATIAL_LAYOUT_UNKNOWN),
      mConvValid(false),
      mStreamBuf(NULL),
      mStreamValid(0),
      mStreamCap(0),
      mInterleaved(NULL),
      mStereoFloat(NULL),
      mStage(NULL),
      mStageValid(0),
      mAnchorTicks(0),
      mHaveAnchorTicks(false),
      mOutFramesEmitted(0),
      mSawInputEos(false),
      mSentOutputEos(false),
      mLastInputTicks(0),
      mFramesQueued(0),
      mInputChannels(6),
      mInputSampleRate(48000),
      mHaveAppliedDownmix(false),
       mDebug(false) {
    for (int i = 0; i < SPATIAL_CH_MAX; ++i) {
        mPlanar[i] = NULL;
    }

    switch (mCodec) {
        case SPATIAL_CODEC_EAC3:
            mInputCoding = OMX_AUDIO_CodingAndroidEAC3;
            break;
        case SPATIAL_CODEC_AC3:
            mInputCoding = OMX_AUDIO_CodingAndroidAC3;
            break;
        case SPATIAL_CODEC_DTS:
        case SPATIAL_CODEC_TRUEHD:
        default:
            mInputCoding = OMX_AUDIO_CodingAutoDetect;
            break;
    }

    // Input port: compressed audio.
    OMX_PARAM_PORTDEFINITIONTYPE inDef;
    OMX_STRING inMime = (OMX_STRING)"audio/ac3";
    switch (mCodec) {
        case SPATIAL_CODEC_EAC3:
            inMime = (OMX_STRING)"audio/eac3";
            break;
        case SPATIAL_CODEC_DTS:
            inMime = (OMX_STRING)"audio/vnd.dts";
            break;
        case SPATIAL_CODEC_TRUEHD:
            inMime = (OMX_STRING)"audio/true-hd";
            break;
        case SPATIAL_CODEC_AC3:
        default:
            inMime = (OMX_STRING)"audio/ac3";
            break;
    }
    InitOMXParams(&inDef);
    inDef.nPortIndex = 0;
    inDef.eDir = OMX_DirInput;
    inDef.nBufferCountMin = kNumInputBuffers;
    inDef.nBufferCountActual = kNumInputBuffers;
    inDef.nBufferSize = kInputBufferSize;
    inDef.bEnabled = OMX_TRUE;
    inDef.bPopulated = OMX_FALSE;
    inDef.eDomain = OMX_PortDomainAudio;
    inDef.format.audio.cMIMEType = inMime;
    inDef.format.audio.pNativeRender = 0;
    inDef.format.audio.bFlagErrorConcealment = OMX_FALSE;
    inDef.format.audio.eEncoding = (OMX_AUDIO_CODINGTYPE)mInputCoding;
    addPort(inDef);

    // Output port: stereo PCM (descriptor answered by internalGetParameter).
    OMX_PARAM_PORTDEFINITIONTYPE outDef;
    InitOMXParams(&outDef);
    outDef.nPortIndex = 1;
    outDef.eDir = OMX_DirOutput;
    outDef.nBufferCountMin = kNumOutputBuffers;
    outDef.nBufferCountActual = kNumOutputBuffers;
    outDef.nBufferSize = kOutputBufferSize;
    outDef.bEnabled = OMX_TRUE;
    outDef.bPopulated = OMX_FALSE;
    outDef.eDomain = OMX_PortDomainAudio;
    outDef.format.audio.cMIMEType = (OMX_STRING)"audio/raw";
    outDef.format.audio.pNativeRender = 0;
    outDef.format.audio.bFlagErrorConcealment = OMX_FALSE;
    outDef.format.audio.eEncoding = OMX_AUDIO_CodingPCM;
    addPort(outDef);

    // Pre-allocate everything the audio path needs (realtime safety: no
    // first-use malloc while decoding).
    mStreamBuf = (uint8_t *)malloc(kStreamBufferSize);
    mInterleaved = (float *)malloc(kMaxFrameSamples * kMaxChannels * sizeof(float));
    mStereoFloat = (float *)malloc(kMaxFrameSamples * kOutChannels * sizeof(float));
    mStage = (int16_t *)malloc(kStageFrames * kOutChannels * sizeof(int16_t));
    bool planesOk = true;
    for (int i = 0; i < SPATIAL_CH_MAX; ++i) {
        mPlanar[i] = (float *)malloc(kMaxFrameSamples * sizeof(float));
        planesOk = planesOk && (mPlanar[i] != NULL);
    }
    if (mStreamBuf == NULL || mInterleaved == NULL || mStereoFloat == NULL
            || mStage == NULL || !planesOk) {
        ALOGE("SpatialOmxAdapter: scratch allocation failed");
        mInitCheck = OMX_ErrorInsufficientResources;
        return;
    }
    mStreamCap = kStreamBufferSize;

    if (!ensurePipeline()) {
        ALOGE("SpatialOmxAdapter: pipeline creation failed");
        mInitCheck = OMX_ErrorInsufficientResources;
        return;
    }

    char debugProp[PROPERTY_VALUE_MAX];
    mDebug = property_get("persist.vendor.spatialdm.debug", debugProp, "0") > 0
            && atoi(debugProp) != 0;
    if (mDebug) {
        ALOGI("Created SpatialOmxAdapter codec=%s role=%s",
                spatial_decoder_get_codec_name(mCodec), mRole);
    }
}

SpatialOmxAdapter::~SpatialOmxAdapter() {
    releasePipeline();
    free(mStreamBuf);
    free(mInterleaved);
    free(mStereoFloat);
    free(mStage);
    for (int i = 0; i < SPATIAL_CH_MAX; ++i) {
        free(mPlanar[i]);
    }
}

OMX_ERRORTYPE SpatialOmxAdapter::initCheck() const {
    return mInitCheck;
}

bool SpatialOmxAdapter::ensurePipeline() {
    if (mDecoder != NULL) {
        return true;
    }
    spatial_decoder_config_t config;
    memset(&config, 0, sizeof(config));
    config.codec = mCodec;
    config.sample_rate = kOutSampleRate;
    config.layout = SPATIAL_LAYOUT_UNKNOWN;
    config.num_channels = 0;

    mDecoder = spatial_decoder_create(&config);
    if (mDecoder == NULL) {
        ALOGE("spatial_decoder_create failed");
        return false;
    }
    if (spatial_decoder_open(mDecoder) != SPATIAL_DECODER_OK) {
        ALOGE("spatial_decoder_open failed: %s",
                spatial_decoder_strerror(SPATIAL_DECODER_ERROR_OPEN_FAILED));
        spatial_decoder_destroy(mDecoder);
        mDecoder = NULL;
        return false;
    }

    spatial_downmix_config_t dmConfig;
    spatial_downmix_get_default_config_5_1(&dmConfig);
    mDownmix = spatial_downmix_create(&dmConfig);
    if (mDownmix == NULL) {
        ALOGE("spatial_downmix_create failed");
        spatial_decoder_close(mDecoder);
        spatial_decoder_destroy(mDecoder);
        mDecoder = NULL;
        return false;
    }

    mPropCtx = spatial_property_create();
    // Property context is optional; defaults apply without it.
    refreshConfigIfNeeded(true);
    return true;
}

void SpatialOmxAdapter::releasePipeline() {
    if (mPcm != NULL) {
        spatial_pcm_converter_destroy(mPcm);
        mPcm = NULL;
    }
    mConvValid = false;
    if (mDownmix != NULL) {
        spatial_downmix_destroy(mDownmix);
        mDownmix = NULL;
    }
    if (mPropCtx != NULL) {
        spatial_property_destroy(mPropCtx);
        mPropCtx = NULL;
    }
    if (mDecoder != NULL) {
        spatial_decoder_close(mDecoder);
        spatial_decoder_destroy(mDecoder);
        mDecoder = NULL;
    }
}

// Latch the output-timeline anchor from the first input PTS carrying a
// real value (NOPTS/INT64_MIN carries no timing information).
void SpatialOmxAdapter::noteInputTimestamp(OMX_TICKS ticks) {
    if (!mHaveAnchorTicks && ticks != (OMX_TICKS)INT64_MIN) {
        mAnchorTicks = (int64_t)ticks;
        mHaveAnchorTicks = true;
    }
}

void SpatialOmxAdapter::resetStreamState() {
    mStreamValid = 0;
    mStageValid = 0;
    mAnchorTicks = 0;
    mHaveAnchorTicks = false;
    mOutFramesEmitted = 0;
    mSawInputEos = false;
    mSentOutputEos = false;
    mConvValid = false;
    if (mPcm != NULL) {
        spatial_pcm_converter_destroy(mPcm);
        mPcm = NULL;
    }
    if (mDecoder != NULL) {
        spatial_decoder_flush(mDecoder);
    }
}

// ---------------------------------------------------------------------------
// Parameter handling. Mirrors the reference dispatch:
//   Role, PCM (output), Android AC3/EAC3, three MTK vendor indices;
//   everything else delegates to the framework base.
// ---------------------------------------------------------------------------

OMX_ERRORTYPE SpatialOmxAdapter::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    if (params == NULL) {
        return OMX_ErrorBadParameter;
    }

    switch ((uint32_t)index) {
        case OMX_IndexParamStandardComponentRole: {
            OMX_PARAM_COMPONENTROLETYPE *role =
                    (OMX_PARAM_COMPONENTROLETYPE *)params;
            if (!isValidOMXParam(role)) {
                return OMX_ErrorBadParameter;
            }
            strncpy((char *)role->cRole, mRole, OMX_MAX_STRINGNAME_SIZE - 1);
            role->cRole[OMX_MAX_STRINGNAME_SIZE - 1] = '\0';
            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioPcm: {
            OMX_AUDIO_PARAM_PCMMODETYPE *pcm =
                    (OMX_AUDIO_PARAM_PCMMODETYPE *)params;
            if (!isValidOMXParam(pcm)) {
                return OMX_ErrorBadParameter;
            }
            if (pcm->nPortIndex != 1) {
                return OMX_ErrorBadParameter;
            }
            // Stereo 16-bit signed LE interleaved PCM @ 48 kHz.
            // Byte-exact match of the reference component's answer.
            pcm->nChannels = kOutChannels;
            pcm->eNumData = OMX_NumericalDataSigned;
            pcm->eEndian = OMX_EndianLittle;
            pcm->bInterleaved = OMX_TRUE;
            pcm->nBitPerSample = 16;
            pcm->nSamplingRate = kOutSampleRate;
            pcm->ePCMMode = OMX_AUDIO_PCMModeLinear;
            pcm->eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            pcm->eChannelMapping[1] = OMX_AUDIO_ChannelRF;
            for (int i = 2; i < OMX_AUDIO_MAXCHANNELS; ++i) {
                pcm->eChannelMapping[i] = OMX_AUDIO_ChannelNone;
            }
            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAndroidAc3: {
            if (mCodec != SPATIAL_CODEC_AC3) {
                break;
            }
            OMX_AUDIO_PARAM_ANDROID_AC3TYPE *ac3 =
                    (OMX_AUDIO_PARAM_ANDROID_AC3TYPE *)params;
            if (!isValidOMXParam(ac3)) {
                return OMX_ErrorBadParameter;
            }
            if (ac3->nPortIndex != 0) {
                return OMX_ErrorBadParameter;
            }
            ac3->nChannels = mInputChannels;
            ac3->nSampleRate = mInputSampleRate;
            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAndroidEac3: {
            if (mCodec != SPATIAL_CODEC_EAC3) {
                break;
            }
            OMX_AUDIO_PARAM_ANDROID_EAC3TYPE *eac3 =
                    (OMX_AUDIO_PARAM_ANDROID_EAC3TYPE *)params;
            if (!isValidOMXParam(eac3)) {
                return OMX_ErrorBadParameter;
            }
            if (eac3->nPortIndex != 0) {
                return OMX_ErrorBadParameter;
            }
            eac3->nChannels = mInputChannels;
            eac3->nSampleRate = mInputSampleRate;
            return OMX_ErrorNone;
        }

        case kMtkIndexAc3:
        case kMtkIndexDts:
        case kMtkIndexTrueHd: {
            // MTK vendor audio descriptors. Layout-compatible prefix with
            // the standard audio param structs: +0 nSize, +4 nVersion,
            // +8 nPortIndex, +12 nChannels, +16 nSampleRate.
            OMX_U32 *words = (OMX_U32 *)params;
            if (words[0] < 24) {  // need nSize/nVersion/nPortIndex/nChannels/nSampleRate
                return OMX_ErrorBadParameter;
            }
            if (words[2] != 0) {  // input port only
                return OMX_ErrorBadParameter;
            }
            words[3] = mInputChannels;
            words[4] = mInputSampleRate;
            return OMX_ErrorNone;
        }

        default:
            break;
    }

    return SimpleSoftOMXComponent::internalGetParameter(index, params);
}

OMX_ERRORTYPE SpatialOmxAdapter::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    if (params == NULL) {
        return OMX_ErrorBadParameter;
    }

    switch ((uint32_t)index) {
        case OMX_IndexParamStandardComponentRole: {
            const OMX_PARAM_COMPONENTROLETYPE *role =
                    (const OMX_PARAM_COMPONENTROLETYPE *)params;
            if (!isValidOMXParam(role)) {
                return OMX_ErrorBadParameter;
            }
            if (!isRoleForCodec((const char *)role->cRole, mCodec)) {
                ALOGW("SpatialOmxAdapter: rejecting role '%s' (ours is '%s')",
                        (const char *)role->cRole, mRole);
                return OMX_ErrorBadParameter;
            }
            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioPcm: {
            const OMX_AUDIO_PARAM_PCMMODETYPE *pcm =
                    (const OMX_AUDIO_PARAM_PCMMODETYPE *)params;
            if (!isValidOMXParam(pcm)) {
                return OMX_ErrorBadParameter;
            }
            if (pcm->nPortIndex != 1) {
                return OMX_ErrorBadParameter;
            }
            // Fixed stereo output; accept (and ignore) the framework's
            // echo as long as it targets the output port.
            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAndroidAc3:
        case OMX_IndexParamAudioAndroidEac3:
        case kMtkIndexAc3:
        case kMtkIndexDts:
        case kMtkIndexTrueHd: {
            // Store the input descriptor echoed back by GetParameter.
            const OMX_U32 *words = (const OMX_U32 *)params;
            if (words[0] < 24) {
                return OMX_ErrorBadParameter;
            }
            if (words[2] != 0) {
                return OMX_ErrorBadParameter;
            }
            OMX_U32 ch = words[3];
            OMX_U32 rate = words[4];
            if (ch >= 1 && ch <= kMaxChannels) {
                mInputChannels = ch;
            }
            if (rate >= 8000 && rate <= 192000) {
                mInputSampleRate = rate;
            }
            return OMX_ErrorNone;
        }

        default:
            break;
    }

    return SimpleSoftOMXComponent::internalSetParameter(index, params);
}

// ---------------------------------------------------------------------------
// Buffer flow.
// ---------------------------------------------------------------------------

void SpatialOmxAdapter::onQueueFilled(OMX_U32 portIndex) {
    if (mInitCheck != OMX_ErrorNone) {
        notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
        return;
    }
    if (!ensurePipeline()) {
        notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
        return;
    }
    ++mFramesQueued;

    if (portIndex == 0) {
        // Ingest all queued input buffers.
        List<BufferInfo *> &inQueue = getPortQueue(0);
        while (!inQueue.empty()) {
            List<BufferInfo *>::iterator it = inQueue.begin();
            BufferInfo *info = *it;
            inQueue.erase(it);

            OMX_BUFFERHEADERTYPE *header = info->mHeader;
            if (header == NULL) {
                continue;
            }
            if ((header->nFlags & OMX_BUFFERFLAG_EOS) != 0) {
                mSawInputEos = true;
            }
            if (header->nFilledLen > 0) {
                size_t need = mStreamValid + header->nFilledLen;
                if (need > mStreamCap) {
                    size_t newCap = mStreamCap * 2;
                    if (newCap < need) {
                        newCap = need;
                    }
                    uint8_t *grown = (uint8_t *)realloc(mStreamBuf, newCap);
                    if (grown == NULL) {
                        ALOGE("SpatialOmxAdapter: stream buffer grow failed");
                        info->mOwnedByUs = false;
                        notifyEmptyBufferDone(header);
                        notify(OMX_EventError,
                                OMX_ErrorInsufficientResources, 0, NULL);
                        return;
                    }
                    mStreamBuf = grown;
                    mStreamCap = newCap;
                }
                memcpy(mStreamBuf + mStreamValid,
                        header->pBuffer + header->nOffset,
                        header->nFilledLen);
                mStreamValid += header->nFilledLen;
                mLastInputTicks = header->nTimeStamp;
                noteInputTimestamp(header->nTimeStamp);
            } else if ((header->nFlags & OMX_BUFFERFLAG_EOS) != 0) {
                mLastInputTicks = header->nTimeStamp;
                noteInputTimestamp(header->nTimeStamp);
            }
            // MTK's SimpleSoftOMXComponent never clears mOwnedByUs on the
            // return path (machine-code verified: one SET in
            // onMessageReceived, zero clears; notify = direct callback),
            // so a client resubmit after EmptyBufferDone would trip
            // CHECK(!mOwnedByUs). The system reference soft codecs recycle
            // fine, hence clear component-side on return, before notify.
            info->mOwnedByUs = false;
            notifyEmptyBufferDone(header);
        }

        decodeAvailableFrames();
    }

    drainStageToOutput(false);
}

void SpatialOmxAdapter::decodeAvailableFrames() {
    // Feed the accumulated stream to the decoder (sliding window: the
    // decoder reports how many bytes it consumed, like the decoder_test
    // harness). Parser lookahead windowing (BLOCKER-2): draining the
    // stream buffer on every input locks send boundaries to the client's
    // chunking; when that locks to frame boundaries (e.g. production
    // 1-frame extractor samples) parser lookahead starves and yield
    // halves. Offer at most kParserPieceBytes per send and hold a
    // sub-piece remainder for the next input, so piece boundaries drift
    // across frames. EOS flushes everything. Byte order, ownership,
    // timestamps and the EOS drop below are unchanged.
    while (mStreamValid > 0
            && (mSawInputEos || mStreamValid >= kParserPieceBytes)) {
        size_t piece = mStreamValid < (size_t)kParserPieceBytes
                ? mStreamValid : (size_t)kParserPieceBytes;
        int consumed = spatial_decoder_send_packet(
                mDecoder, mStreamBuf, piece, (int64_t)mLastInputTicks);
        if (consumed < 0) {
            if (consumed == SPATIAL_DECODER_AGAIN) {
                break;  // need more data
            }
            ALOGW("SpatialOmxAdapter: send_packet failed: %s",
                    spatial_decoder_strerror(consumed));
            // Drop the stream to avoid a stall; the next input
            // continues from a fresh sync point.
            mStreamValid = 0;
            break;
        }
        if (consumed == 0) {
            // No parser progress this call (partial frame at the stream
            // end, or an EAGAIN stop with the decoder backed up): drain
            // anything pending before yielding so a full decoder can
            // never wedge the stream with no drain path.
            drainDecoderFrames();
            break;
        }
        mStreamValid -= (size_t)consumed;
        if (mStreamValid > 0) {
            memmove(mStreamBuf, mStreamBuf + consumed, mStreamValid);
        }

        drainDecoderFrames();
    }

    // Tail staged retry: a final EAGAIN stop can leave a staged packet
    // with an empty stream, in which case no further send call would
    // ever retry it. One sizeless send flushes it (rejected with -1
    // when nothing is staged; the return is intentionally ignored).
    if (mStreamValid == 0) {
        spatial_decoder_send_packet(mDecoder, mStreamBuf, 0,
                (int64_t)mLastInputTicks);
    }

    // Tail drain: a send that consumes the final stream bytes can leave
    // decoded frames pending with no further input coming (stream EOS).
    // One unconditional drain keeps the tail (including EOS-adjacent
    // frames) flowing; it no-ops when the decoder is empty.
    drainDecoderFrames();

    refreshConfigIfNeeded(false);
}

void SpatialOmxAdapter::drainDecoderFrames() {
    spatial_frame_t frame;
    for (;;) {
        memset(&frame, 0, sizeof(frame));
        int ret = spatial_decoder_receive_frame(mDecoder, &frame);
        if (ret == SPATIAL_DECODER_AGAIN) {
            break;
        }
        if (ret == SPATIAL_DECODER_ERROR_EOF) {
            break;
        }
        if (ret != SPATIAL_DECODER_OK) {
            ALOGW("SpatialOmxAdapter: receive_frame failed: %s",
                    spatial_decoder_strerror(ret));
            break;
        }
        if (frame.nb_samples <= 0 || frame.num_channels <= 0) {
            continue;
        }
        processFrame(&frame);
    }
}

bool SpatialOmxAdapter::ensureConverter(
        spatial_sample_fmt_t fmt, int rate, int nch, spatial_layout_t layout) {
    if (mConvValid && mConvInFmt == fmt && mConvInRate == rate
            && mConvInCh == nch && mConvInLayout == layout) {
        return true;
    }
    if (mPcm != NULL) {
        spatial_pcm_converter_destroy(mPcm);
        mPcm = NULL;
    }
    mConvValid = false;

    spatial_pcm_format_t inFmt;
    memset(&inFmt, 0, sizeof(inFmt));
    inFmt.format = fmt;
    inFmt.sample_rate = rate;
    inFmt.layout = layout;
    inFmt.num_channels = (uint8_t)nch;
    for (int i = 0; i < nch && i < SPATIAL_CH_MAX; ++i) {
        inFmt.channel_map[i] = (uint8_t)i;
    }

    spatial_pcm_format_t outFmt = inFmt;
    outFmt.format = SPATIAL_SAMPLE_FMT_FLTP;
    outFmt.sample_rate = kOutSampleRate;

    mPcm = spatial_pcm_converter_create(&inFmt, &outFmt);
    if (mPcm == NULL) {
        ALOGE("SpatialOmxAdapter: PCM converter creation failed");
        return false;
    }
    mConvInFmt = fmt;
    mConvInRate = rate;
    mConvInCh = nch;
    mConvInLayout = layout;
    mConvValid = true;
    return true;
}

void SpatialOmxAdapter::processFrame(const spatial_frame_t *frame) {
    int nch = frame->num_channels;
    if (nch < 1 || nch > kMaxChannels) {
        ALOGW("SpatialOmxAdapter: bad channel count %d", nch);
        return;
    }
    int rate = frame->sample_rate > 0 ? frame->sample_rate : kOutSampleRate;
    spatial_layout_t layout = spatial_layout_from_channel_count(nch);

    // Track the input geometry for GetParameter echoes.
    if ((OMX_U32)nch != mInputChannels) {
        mInputChannels = (OMX_U32)nch;
    }
    if (rate >= 8000 && rate <= 192000
            && (OMX_U32)rate != mInputSampleRate) {
        mInputSampleRate = (OMX_U32)rate;
    }

    if (!ensureConverter(frame->format, rate, nch, layout)) {
        return;
    }

    int remaining = frame->nb_samples;
    int offset = 0;
    const int bytesPerSample =
            spatial_pcm_format_get_bytes_per_sample(frame->format);

    while (remaining > 0) {
        int chunk = remaining > kMaxFrameSamples ? kMaxFrameSamples : remaining;

        const uint8_t *inPtrs[SPATIAL_CH_MAX];
        uint8_t *outPtrs[SPATIAL_CH_MAX];
        bool planarIn =
                spatial_pcm_format_is_planar(frame->format) != 0;
        for (int c = 0; c < nch; ++c) {
            if (planarIn) {
                inPtrs[c] = frame->data[c] == NULL ? NULL
                        : (const uint8_t *)frame->data[c] + offset * bytesPerSample;
            } else {
                inPtrs[c] = NULL;  // interleaved uses data[0] below
            }
            outPtrs[c] = (uint8_t *)mPlanar[c];
        }
        if (!planarIn) {
            // Interleaved input lives in data[0]; describe per-channel
            // offsets through the single base pointer.
            const uint8_t *base = frame->data[0] == NULL ? NULL
                    : (const uint8_t *)frame->data[0]
                      + (size_t)offset * (size_t)nch * (size_t)bytesPerSample;
            for (int c = 0; c < nch; ++c) {
                inPtrs[c] = base;
            }
        }

        if (spatial_pcm_converter_process(
                    mPcm, inPtrs, outPtrs, chunk) != 0) {
            ALOGW("SpatialOmxAdapter: PCM convert failed");
            return;
        }

        // Expected resampled length (mirrors the converter's formula).
        double ratio = (double)kOutSampleRate / (double)rate;
        int outSamples = (rate == kOutSampleRate)
                ? chunk : (int)(chunk * ratio) + 1;
        if (outSamples > kMaxFrameSamples) {
            outSamples = kMaxFrameSamples;
        }

        if (nch == kOutChannels) {
            // Stereo: straight to float stereo (no downmix matrix).
            for (int i = 0; i < outSamples; ++i) {
                mStereoFloat[i * 2 + 0] = mPlanar[0][i];
                mStereoFloat[i * 2 + 1] = mPlanar[1][i];
            }
        } else {
            if (layout != SPATIAL_LAYOUT_5_1
                    && layout != SPATIAL_LAYOUT_7_1) {
                // Non-surround multichannel: first two channels.
                for (int i = 0; i < outSamples; ++i) {
                    mStereoFloat[i * 2 + 0] = mPlanar[0][i];
                    mStereoFloat[i * 2 + 1] =
                            nch > 1 ? mPlanar[1][i] : mPlanar[0][i];
                }
            } else {
                for (int i = 0; i < outSamples; ++i) {
                    for (int c = 0; c < nch; ++c) {
                        mInterleaved[i * nch + c] = mPlanar[c][i];
                    }
                }
                int chMap[SPATIAL_CH_MAX];
                for (int c = 0; c < nch; ++c) {
                    chMap[c] = frame->channel_map[c] < SPATIAL_CH_MAX
                            ? frame->channel_map[c] : c;
                }
                spatial_downmix_set_channel_map(mDownmix, chMap, nch);
                if (spatial_downmix_process(
                            mDownmix, mInterleaved, mStereoFloat,
                            (size_t)outSamples) != 0) {
                    ALOGW("SpatialOmxAdapter: downmix failed");
                    return;
                }
            }
        }

        // Stage S16 (drop on overflow; never stall the audio path).
        if ((size_t)outSamples > kStageFrames - mStageValid) {
            ALOGW("SpatialOmxAdapter: output stage overflow, dropping %d frames",
                    outSamples);
            return;
        }
        floatToS16Interleaved(
                mStereoFloat, (size_t)outSamples,
                mStage + mStageValid * kOutChannels);
        mStageValid += (size_t)outSamples;

        remaining -= chunk;
        offset += chunk;
    }
}

void SpatialOmxAdapter::floatToS16Interleaved(
        const float *stereo, size_t frames, int16_t *out) {
    for (size_t i = 0; i < frames * kOutChannels; ++i) {
        float v = stereo[i];
        if (v >= 1.0f) {
            out[i] = 32767;
        } else if (v <= -1.0f) {
            out[i] = -32768;
        } else {
            out[i] = (int16_t)(v * 32767.0f);
        }
    }
}

void SpatialOmxAdapter::drainStageToOutput(bool forceEos) {
    // EOS leftover drop: when input is exhausted but the parser cannot
    // consume the residual tail bytes (unparseable partial at stream
    // end), carrying them forever would block EOS propagation. A real
    // decoder discards trailing partials at EOS. This runs after
    // decodeAvailableFrames fully worked the stream this call, so any
    // remainder is proven unparseable with available data (no more
    // input will ever arrive). Mid-stream (no EOS seen) leftovers
    // are always kept for future input.
    if (mSawInputEos && mStreamValid > 0) {
        ALOGI("SpatialOmxAdapter: dropping %lu unparseable tail bytes at EOS",
                (unsigned long)mStreamValid);
        mStreamValid = 0;
    }
    List<BufferInfo *> &outQueue = getPortQueue(1);
    List<BufferInfo *>::iterator it = outQueue.begin();
    while (it != outQueue.end() && mStageValid > 0) {
        BufferInfo *info = *it;
        List<BufferInfo *>::iterator next = it;
        ++next;

        OMX_BUFFERHEADERTYPE *header = info->mHeader;
        if (header != NULL && header->pBuffer != NULL) {
            size_t capacityFrames =
                    header->nAllocLen / (kOutChannels * sizeof(int16_t));
            size_t take = mStageValid < capacityFrames
                    ? mStageValid : capacityFrames;
            memcpy(header->pBuffer, mStage,
                    take * kOutChannels * sizeof(int16_t));
            header->nOffset = 0;
            header->nFilledLen =
                    (OMX_U32)(take * kOutChannels * sizeof(int16_t));
            // Fully computed output timeline: anchor (first valid input
            // PTS, usually stream start) plus actually-emitted frames.
            // Monotonic and rate-correct by construction, independent of
            // NOPTS frame timestamps from the decoder.
            header->nTimeStamp = (OMX_TICKS)(mAnchorTicks +
                    (int64_t)((mOutFramesEmitted * 1000000ULL)
                            / (uint64_t)kOutSampleRate));
            mOutFramesEmitted += take;
            header->nFlags = 0;

            size_t left = mStageValid - take;
            if (left > 0) {
                memmove(mStage, mStage + take * kOutChannels,
                        left * kOutChannels * sizeof(int16_t));
            }
            mStageValid = left;

            bool last = (mSawInputEos && mStreamValid == 0
                    && mStageValid == 0 && !mSentOutputEos)
                    || forceEos;
            if (last) {
                header->nFlags |= OMX_BUFFERFLAG_EOS;
                mSentOutputEos = true;
            }

            outQueue.erase(it);
            // Same ownership release as the input path (see onQueueFilled):
            // the framework never clears mOwnedByUs on return.
            info->mOwnedByUs = false;
            notifyFillBufferDone(header);
        }
        it = next;
    }

    // Empty EOS propagation when no output buffer was available to carry it.
    if (mSawInputEos && mStreamValid == 0 && mStageValid == 0
            && !mSentOutputEos) {
        List<BufferInfo *>::iterator eit = outQueue.begin();
        if (eit != outQueue.end()) {
            BufferInfo *info = *eit;
            OMX_BUFFERHEADERTYPE *header =
                    info != NULL ? info->mHeader : NULL;
            if (header != NULL) {
                header->nOffset = 0;
                header->nFilledLen = 0;
                // End-of-stream position on the computed output timeline
                // (NOT input ticks: those live in a different time domain
                // and would break monotonicity).
                header->nTimeStamp = (OMX_TICKS)(mAnchorTicks +
                        (int64_t)((mOutFramesEmitted * 1000000ULL)
                                / (uint64_t)kOutSampleRate));
                header->nFlags = OMX_BUFFERFLAG_EOS;
                mSentOutputEos = true;
                outQueue.erase(eit);
                if (info != NULL) {
                    info->mOwnedByUs = false;
                }
                notifyFillBufferDone(header);
            }
        }
    }
}

void SpatialOmxAdapter::refreshConfigIfNeeded(bool force) {
    if (mPropCtx == NULL || mDownmix == NULL) {
        return;
    }
    // Throttle property reads off the audio path: every 128 queue events
    // or on explicit request. The downmix matrix itself is applied from
    // the cached snapshot (no property access inside the sample loop).
    if (!force && (mFramesQueued % 128) != 0) {
        return;
    }

    char debugProp[PROPERTY_VALUE_MAX];
    mDebug = property_get("persist.vendor.spatialdm.debug", debugProp, "0") > 0
            && atoi(debugProp) != 0;

    spatial_config_t cfg;
    if (spatial_property_read_all(mPropCtx, &cfg) != 0) {
        return;
    }

    // Translate the property snapshot into a downmix config. The active
    // layout follows the decoded stream; gains follow the matching
    // namespace (5.1 vs 7.1).
    spatial_layout_t layout = mConvValid ? mConvInLayout : SPATIAL_LAYOUT_5_1;
    if (layout != SPATIAL_LAYOUT_5_1 && layout != SPATIAL_LAYOUT_7_1) {
        layout = SPATIAL_LAYOUT_5_1;
    }

    spatial_downmix_config_t dm;
    memset(&dm, 0, sizeof(dm));
    if (layout == SPATIAL_LAYOUT_7_1) {
        spatial_downmix_get_default_config_7_1(&dm);
    } else {
        spatial_downmix_get_default_config_5_1(&dm);
    }
    dm.layout = layout;
    dm.gains_5_1.left = cfg.gains_5_1[0];
    dm.gains_5_1.right = cfg.gains_5_1[1];
    dm.gains_5_1.center = cfg.gains_5_1[2];
    dm.gains_5_1.surround = cfg.gains_5_1[3];
    dm.gains_5_1.lfe = cfg.gains_5_1[4];
    dm.gains_7_1.left = cfg.gains_7_1[0];
    dm.gains_7_1.right = cfg.gains_7_1[1];
    dm.gains_7_1.center = cfg.gains_7_1[2];
    dm.gains_7_1.side = cfg.gains_7_1[3];
    dm.gains_7_1.rear = cfg.gains_7_1[4];
    dm.gains_7_1.lfe = cfg.gains_7_1[5];
    dm.matrix_oba = cfg.matrix_oba;
    dm.matrix_cba = cfg.matrix_cba;
    dm.content_type = cfg.content_type;
    dm.debug_enabled = cfg.debug_enabled ? 1 : 0;

    bool changed = !mHaveAppliedDownmix
            || memcmp(&mAppliedDownmix, &dm, sizeof(dm)) != 0;
    if (changed) {
        spatial_downmix_update_config(mDownmix, &dm);
        mAppliedDownmix = dm;
        mHaveAppliedDownmix = true;
        if (mDebug) {
            if (dm.layout == SPATIAL_LAYOUT_5_1) {
                ALOGI("downmix 5.1: L=%.3f R=%.3f C=%.3f S=%.3f LFE=%.3f",
                        dm.gains_5_1.left, dm.gains_5_1.right,
                        dm.gains_5_1.center, dm.gains_5_1.surround,
                        dm.gains_5_1.lfe);
            } else {
                ALOGI("downmix 7.1: L=%.3f R=%.3f C=%.3f Sd=%.3f Rr=%.3f LFE=%.3f",
                        dm.gains_7_1.left, dm.gains_7_1.right,
                        dm.gains_7_1.center, dm.gains_7_1.side,
                        dm.gains_7_1.rear, dm.gains_7_1.lfe);
            }
        }
    }
}

void SpatialOmxAdapter::onPortFlushCompleted(OMX_U32 portIndex) {
    (void)portIndex;
    if (mDecoder != NULL) {
        spatial_decoder_flush(mDecoder);
    }
    resetStreamState();
}

void SpatialOmxAdapter::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
    (void)portIndex;
    (void)enabled;
}

void SpatialOmxAdapter::onReset() {
    resetStreamState();
}

}  // namespace android

// ---------------------------------------------------------------------------
// Factory functions. Signatures must mangle EXACTLY to the reference:
//   _Z22createSoftOMXComponentPKcPK16OMX_CALLBACKTYPEPvPP17OMX_COMPONENTTYPE
//   _ZN7android25createSoftAc3OmxComponentEPKcPK16OMX_CALLBACKTYPEPvPP17OMX_COMPONENTTYPE
// (SoftOMXPlugin dlsyms the mangled global name and takes ownership of the
// returned android::SoftOMXComponent via sp<>.)
// ---------------------------------------------------------------------------

__attribute__((visibility("default")))
android::SoftOMXComponent *createSoftOMXComponent(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component) {
    if (name == NULL || callbacks == NULL || component == NULL) {
        return NULL;
    }
    return new android::SpatialOmxAdapter(name, callbacks, appData, component);
}

namespace android {

__attribute__((visibility("default")))
SoftOMXComponent *createSoftAc3OmxComponent(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component) {
    return ::createSoftOMXComponent(name, callbacks, appData, component);
}

}  // namespace android

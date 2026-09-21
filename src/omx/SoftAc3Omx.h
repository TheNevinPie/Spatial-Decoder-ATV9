#ifndef SOFT_AC3_OMX_H_
#define SOFT_AC3_OMX_H_

#include <media/stagefright/omx/SimpleOMXComponent.h>
#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaSource.h>

#include "spatial_decoder.h"
#include "spatial_pcm.h"
#include "spatial_downmix.h"
#include "spatial_channel_layout.h"

namespace android {

class SoftAc3Omx : public SimpleOMXComponent {
public:
    SoftAc3Omx(const char* name, const OMX_CALLBACKTYPE* callbacks,
               OMX_PTR appData, OMX_COMPONENTTYPE** component);

    virtual OMX_ERRORTYPE internalGetParameter(
            OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE internalSetParameter(
            OMX_INDEXTYPE index, const OMX_PTR params);

    virtual OMX_ERRORTYPE getConfig(
            OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE setConfig(
            OMX_INDEXTYPE index, const OMX_PTR params);

protected:
    virtual ~SoftAc3Omx();

    virtual void onQueueFilled(OMX_U32 portIndex);
    virtual void onReset();

private:
    enum {
        kNumInputBuffers = 4,
        kNumOutputBuffers = 4,
        kInputBufferSize = 64 * 1024,
        kOutputBufferSize = 64 * 1024,
    };

    enum {
        kStateIdle,
        kStateExecuting,
        kStateEos,
    } mState;

    // Stream buffer for chunked input
    static const size_t kStreamBufferSize = 32 * 1024;
    uint8_t* mStreamBuffer;
    size_t mStreamBufferValid;
    size_t mStreamBufferCapacity;

    spatial_decoder_t* mDecoder;
    spatial_downmix_ctx_t* mDownmix;
    spatial_pcm_converter_t* mPcmConverter;
    spatial_pcm_format_t mInputFormat;
    spatial_pcm_format_t mOutputFormat;
    spatial_downmix_config_t mDownmixConfig;

    // PCM conversion buffers
    spatial_frame_t mFrameOut;
    float* mPlanarPtrs[8];
    float mOutBuffers[8][48000];
    float* mOutPtrs[8];

    // Statistics
    uint64_t mFramesDecoded;
    uint64_t mTotalSamples;

    // Initialize decoder and converters
    status_t initDecoder();
    void releaseDecoder();

    // Buffer management
    void readInputData();
    status_t sendPacketToDecoder();
    void drainOutputFrames();

    // PCM conversion and downmix
    status_t convertAndDownmix();

    // OMX buffer handling
    void fillOutputBuffer(float* data, int num_samples);
    void queueInputBuffer(OMX_BUFFERHEADERTYPE* buffer);
    void queueOutputBuffer(OMX_BUFFERHEADERTYPE* buffer);

    // EOS handling
    bool mSawEOS;
    bool mSentEOS;

    // Helper functions
    const char* getMimeTypeFromRole();
    static spatial_codec_t mimeToSpatialCodec(const char* mime);
    static bool isDebugEnabled();
    static void logDownmixConfig(const char* tag, const spatial_downmix_config_t* config);

    DISALLOW_EVIL_CONSTRUCTORS(SoftAc3Omx);
};

}  // namespace android

#endif  // SOFT_AC3_OMX_H_
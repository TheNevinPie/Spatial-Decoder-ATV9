// SpatialOmxAdapter.h — OMX compatibility adapter for libspatialdecoder.
//
// Architecture: this adapter owns ONLY the Android OMX contract
// (lifecycle, ports, buffers, parameters). All codec, PCM, downmix and
// configuration logic lives in libspatialdecoder.
//
// ABI contract (verified against MT5862 reference binary
// libstagefright_soft_ac3dec.so, MD5 5065766ce3ec35d1c9904fb9c66a9066):
//   * SpatialOmxAdapter derives from android::SimpleSoftOMXComponent
//     (framework base, resolved at runtime from libstagefright_omx.so).
//   * Factories (see SpatialOmxAdapter.cpp):
//       ::createSoftOMXComponent (global C++ linkage)
//       android::createSoftAc3OmxComponent
//     both returning android::SoftOMXComponent*.
//   * Output port = stereo 16-bit signed little-endian interleaved PCM,
//     48 kHz (matches reference OMX_AUDIO_PARAM_PCMMODETYPE fill).
//
// Four separate shared libraries are built from the same sources, one per
// codec (ac3dec / eac3dec / dtsdec / truehddec). The per-library
// DEFAULT_CODEC_NAME compile definition selects the fallback codec; the
// primary dispatch is the component name passed by SoftOMXPlugin.

#ifndef SPATIAL_OMX_ADAPTER_H_
#define SPATIAL_OMX_ADAPTER_H_

#include <media/stagefright/omx/SimpleSoftOMXComponent.h>

#include <OMX_Audio.h>
#include <OMX_AudioExt.h>
#include <OMX_IndexExt.h>

#include "spatial_decoder.h"
#include "spatial_pcm.h"
#include "spatial_downmix.h"
#include "spatial_channel_layout.h"
#include "spatial_config.h"
#include "property_bridge.h"

namespace android {

struct SpatialOmxAdapter : public SimpleSoftOMXComponent {
    SpatialOmxAdapter(
            const char *name,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component);

    virtual OMX_ERRORTYPE initCheck() const;

protected:
    virtual ~SpatialOmxAdapter();

    virtual OMX_ERRORTYPE internalGetParameter(
            OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE internalSetParameter(
            OMX_INDEXTYPE index, const OMX_PTR params);

    virtual void onQueueFilled(OMX_U32 portIndex);
    virtual void onPortFlushCompleted(OMX_U32 portIndex);
    virtual void onPortEnableCompleted(OMX_U32 portIndex, bool enabled);
    virtual void onReset();

private:
    enum {
        // Buffer pool sizing is the recycling discipline. The MTK
        // SimpleSoftOMXComponent build on this target aborts re-submit
        // of an already-submitted header (CHECK(!mOwnedByUs), no
        // normal-path clear observed), so this adapter NEVER re-submits:
        // every input chunk and every output fill consumes a FRESH
        // header. Counts must therefore cover the largest stream
        // serviced between flushes (235 input chunks for the 1.9 MB
        // E-AC-3 7.1 vector; 256 gives headroom). Cost: 256 x 64KB per
        // port worst case, committed at AllocateBuffer time. This does
        // NOT scale to arbitrarily long streams (a 2 h movie would need
        // ~175k headers): production use needs either true recycling
        // (framework fix) or periodic flush/reset cycling. Validation
        // vectors (<= 2 MB) fit comfortably.
        kNumInputBuffers = 256,
        kNumOutputBuffers = 256,
        kInputBufferSize = 64 * 1024,
        kOutputBufferSize = 64 * 1024,
        kStreamBufferSize = 32 * 1024,
        // Parser feed piece (BLOCKER-2): the stock FFmpeg parsers only
        // release frame N with multi-frame lookahead. Pump the parser in
        // fixed pieces of this size (a non-multiple of typical frame
        // sizes: 4096 for E-AC-3, 2560 for AC-3) so piece boundaries drift
        // across frames whatever the client's input chunking. Measured:
        // 6000 B pieces recover 312/313 E-AC-3 frames where locked-phase
        // 1-frame feeds yield ~50%. A sub-piece remainder is held for the
        // next input; EOS flushes everything.
        kParserPieceBytes = 6000,
        // Hardened scratch capacities (pre-allocated, never grown on the
        // audio path). spatial_pcm rejects frames > 4096 samples.
        kMaxFrameSamples = 4096,
        kMaxChannels = SPATIAL_CH_MAX,  // 8
        kOutChannels = 2,
        kOutSampleRate = 48000,
        // Stereo S16 staging (frames). Must cover burst decode between
        // output-buffer arrivals.
        kStageFrames = 16384,
    };

    // MTK vendor parameter indices (reverse-engineered from reference
    // internalGetParameter/internalSetParameter dispatch).
    enum {
        kMtkIndexAc3 = 0x07FB0004,
        kMtkIndexDts = 0x07FB0007,
        kMtkIndexTrueHd = 0x07FB0008,
        // Device-observed DTS/TrueHD audio descriptor index (ACodec
        // setupDTSDecoder/setupTrueHDDecoder query it with a 20-byte
        // {nSize, nVersion, nPortIndex, nChannels, nSampleRate} struct).
        // Lives past OMX_IndexExtAudioEndUnused of our 1.1-era headers;
        // the device (IL 1.2-era headers) defines it there.
        kIdxAndroidDts = 0x6F400009,
        // Stock role-table (shared MTK soft binary, all four names) input
        // codings per role: DTS reports 0x6F100003, TrueHD reports
        // 0x6F100001 (= OMX_AUDIO_CodingAndroidAC3, consistent with the
        // audio_decoder.ac3p role alias). Our 1.1-era headers define no
        // DTS/TrueHD codings, so the DTS value is literal.
        kCodingDts = 0x6F100003,
    };

    // ---- Spatial identity cookie (fixed at construction) ----
    // Magic 0x53504154 ("SPAT") as the FIRST subclass member (offset =
    // sizeof(SimpleSoftOMXComponent)). The framework destroy-cave uses it
    // to recognize OUR components (their plugin instance is heap-private
    // and invisible to OMXMaster's tables). Never modified after init.
    uint32_t mSpatialCookie;

    // ---- Codec identity (fixed at construction) ----
    spatial_codec_t mCodec;
    const char *mRole;  // e.g. "audio_decoder.ac3"
    // Input port encoding. Holds an OMX_AUDIO_CODINGTYPE value
    // (AutoDetect/PCM) or an OMX_AUDIO_CODINGEXTTYPE value
    // (AndroidAC3/AndroidEAC3); kept as U32 because the two enums are
    // distinct C++ types.
    OMX_U32 mInputCoding;

    OMX_ERRORTYPE mInitCheck;

    // ---- libspatialdecoder pipeline ----
    spatial_decoder_t *mDecoder;
    spatial_pcm_converter_t *mPcm;  // multichannel -> FLTP 48 kHz, same layout
    spatial_downmix_ctx_t *mDownmix;  // multichannel float -> stereo float
    spatial_property_ctx_t *mPropCtx;

    // Geometry the PCM converter was built for (recreate on change).
    spatial_sample_fmt_t mConvInFmt;
    int mConvInRate;
    int mConvInCh;
    spatial_layout_t mConvInLayout;
    bool mConvValid;

    // ---- Stream accumulation (compressed input) ----
    uint8_t *mStreamBuf;
    size_t mStreamValid;
    size_t mStreamCap;

    // ---- Scratch (pre-allocated) ----
    float *mPlanar[SPATIAL_CH_MAX];  // converter output planes (FLTP 48 kHz)
    float *mInterleaved;  // multichannel interleaved float (downmix input)
    float *mStereoFloat;  // stereo float (downmix output)

    // ---- Stereo S16 output staging ----
    int16_t *mStage;
    size_t mStageValid;  // frames staged

    // ---- Output timestamp timeline (sample-count-derived) ----
    // Output PTS = anchor + emitted * 1e6 / sample_rate. The anchor comes
    // from the first input PTS with a valid value; the advance follows
    // actually-emitted frames, so the timeline is monotonic and
    // rate-correct even when decoded frame PTS values are NOPTS (which
    // FFmpeg emits, e.g. before parser timestamp tracking engages).
    // Reset together with stream state (seek/flush restarts the timeline).
    int64_t mAnchorTicks;
    bool mHaveAnchorTicks;
    uint64_t mOutFramesEmitted;  // stereo S16 frames handed to OMX so far

    // ---- Stream state ----
    bool mSawInputEos;
    bool mSentOutputEos;
    OMX_TICKS mLastInputTicks;
    uint64_t mFramesQueued;  // onQueueFilled call count (config throttle)

    // ---- Input descriptor state (echoed by GetParameter) ----
    OMX_U32 mInputChannels;  // default 6
    OMX_U32 mInputSampleRate;  // default 48000

    // ---- Output PCM echo state (stored on Set, echoed on Get) ----
    // ACodec SETs the output PCM descriptor from its own format and may
    // GET it back to verify; stock components echo stored values.
    OMX_U32 mOutChannels;  // default 2 (stereo downmix)
    OMX_U32 mOutSampleRate;  // default 48000

    // ---- Downmix config cache (change-triggered logging) ----
    spatial_downmix_config_t mAppliedDownmix;
    bool mHaveAppliedDownmix;
    bool mDebug;

    // Helpers.
    static spatial_codec_t componentNameToSpatialCodec(const char *name);
    static const char *roleForCodec(spatial_codec_t codec);
    static bool isRoleForCodec(const char *role, spatial_codec_t codec);

    bool ensurePipeline();
    void releasePipeline();
    void resetStreamState();

    // (Re)build the PCM converter for the observed frame geometry.
    bool ensureConverter(
            spatial_sample_fmt_t fmt, int rate, int nch, spatial_layout_t layout);

    // Latch the output-timeline anchor from input timestamps.
    void noteInputTimestamp(OMX_TICKS ticks);

    // Re-read properties (throttled) and apply changed downmix config.
    void refreshConfigIfNeeded(bool force);

    // Decode one frame burst from the accumulated stream into staging.
    void decodeAvailableFrames();

    // Drain all currently available decoder frames into staging.
    void drainDecoderFrames();

    // Process a single decoded frame into the S16 stage (chunked <= 4k).
    void processFrame(const spatial_frame_t *frame);

    // Move staged S16 stereo into queued output buffers.
    void drainStageToOutput(bool forceEos);

    static void floatToS16Interleaved(
            const float *stereo, size_t frames, int16_t *out);

    DISALLOW_EVIL_CONSTRUCTORS(SpatialOmxAdapter);
};

}  // namespace android

#endif  // SPATIAL_OMX_ADAPTER_H_

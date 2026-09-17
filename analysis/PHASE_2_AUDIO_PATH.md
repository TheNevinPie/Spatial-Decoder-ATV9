# Phase 2: Actual Audio Path Reconstruction

## Verified Execution Graph

Based on forensic analysis of binaries, configurations, and system properties:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        APPLICATION LAYER                                    │
│  (MediaPlayer, ExoPlayer, SmartTube, Kodi, etc.)                           │
│       │                                                                     │
│       ▼                                                                     │
│  MediaExtractorFactory                                                     │
│       │                                                                     │
│       ▼                                                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ MTK EXTRACTORS (sniff by file signature)                            │   │
│  │  • libac3.mtk.so  → audio/ac3, audio/eac3                          │   │
│  │  • libdts.mtk.so  → audio/vnd.dts, audio/vnd.dts.hd, etc.          │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│       │                                                                     │
│       ▼                                                                     │
│  MediaCodecList (built from media_codecs.xml)                              │
│       │                                                                     │
│       ▼                                                                     │
│  Codec Selection: OMX.google.ac3.decoder / OMX.google.eac3.decoder         │
│              / OMX.google.dts.decoder / OMX.google.truehd.decoder          │
│       │                                                                     │
│       ▼                                                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ SoftOMXPlugin (libstagefright_omx.so)                               │   │
│  │  enumerateComponents() → "OMX.google.ac3.decoder", etc.             │   │
│  │  makeComponentInstance() → createSoftOMXComponent("ac3dec")         │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│       │                                                                     │
│       ▼                                                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ UNIFIED DECODER: libstagefright_soft_ac3dec.so (58KB)               │   │
│  │  SoftAc3Omx class handles ALL formats:                              │   │
│  │  • AC-3 (audio/ac3)                                                │   │
│  │  • E-AC-3 (audio/eac3, audio/eac3-joc)                             │   │
│  │  • DTS (audio/vnd.dts)                                             │   │
│  │  • DTS-HD (audio/vnd.dts.hd)                                       │   │
│  │  • DTS-ES (audio/vnd.dts.hd;profile=lbr)                           │   │
│  │  • DTS-LBR (audio/vnd.dts.lbr)                                     │   │
│  │  • TrueHD (audio/true-hd)                                          │   │
│  │                                                                     │   │
│  │  Internal: FFmpeg libavcodec + libswresample                       │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│       │                                                                     │
│       ▼                                                                     │
│  PCM Output from decoder:                                                  │
│  • Sample format: Planar float (AV_SAMPLE_FMT_FLTP) or int16               │
│  • Sample rate: Stream native (typically 48kHz)                            │
│  • Channel layout: Stream native (5.1, 7.1, stereo)                        │
│  • Channels: Up to 8 (per media_codecs.xml limits)                         │
│       │                                                                     │
│       ▼                                                                     │
│  libswresample (swr_init)                                                  │
│  • Resampling if needed                                                    │
│  • Channel layout conversion                                               │
│  • Sample format conversion → Android PCM (typically 16-bit interleaved)   │
│       │                                                                     │
│       ▼                                                                     │
│  AudioTrack (STREAM_MUSIC or STREAM_DEFAULT)                               │
│  • Write PCM frames                                                        │
│       │                                                                     │
│       ▼                                                                     │
│  AudioFlinger Mixer                                                        │
│  • Mixes all audio streams                                                 │
│  • **Primary mix port: STEREO ONLY (48kHz, 16-bit)** per policy            │
│       │                                                                     │
│       ▼                                                                     │
│  Audio Policy (audio_policy_configuration.xml)                             │
│  • Routes "primary output" → Speaker device                                │
│  • Speaker device profile: PCM 16-bit, 48kHz, STEREO                       │
│       │                                                                     │
│       ▼                                                                     │
│  Audio HAL (audio.primary.mt5862.so)                                       │
│  • out_write() receives STEREO PCM                                         │
│  • Hardware path: MTK MI_AUDIO / MI_PCM                                    │
│       │                                                                     │
│       ▼                                                                     │
│  PHYSICAL OUTPUT: 2.0 Stereo (TV speakers)                                 │
```

## Critical Finding: The Downmix Gap

**The current system has a fundamental architectural issue:**

1. **Decoder outputs multichannel PCM** (5.1, 7.1, etc.)
2. **AudioTrack writes multichannel PCM** (if configured)
3. **AudioFlinger primary mix port is STEREO ONLY** (per audio_policy_configuration.xml)
4. **Speaker device profile is STEREO ONLY**

This means one of two things happens:
- **Scenario A**: AudioFlinger downmixes automatically (using libdownmix.so) when mixing multichannel → stereo
- **Scenario B**: The decoder/HAL forces stereo output before AudioTrack
- **Scenario C**: Multichannel content falls back to compressed passthrough (offload path)

**Evidence for Scenario A**: `audio_effects.xml` loads `libdownmix.so` as a standard effect.

**Evidence for Scenario B**: The unified decoder has `ensureResampler()` and `flushResamplerToPcm()` methods that use `swr_init` - it CAN do channel conversion.

**Evidence for Scenario C**: The offload output path in audio_policy_configuration.xml supports all compressed formats with full channel masks up to 7.1.

## Channel Mapping Analysis

### Decoder Output Channel Order (FFmpeg Standard)
FFmpeg uses standard channel layouts:
- **5.1**: FL, FR, FC, LFE, SL, SR (AV_CH_LAYOUT_5POINT1)
- **7.1**: FL, FR, FC, LFE, SL, SR, BL, BR (AV_CH_LAYOUT_7POINT1)

### Android AudioTrack Channel Masks
- `AUDIO_CHANNEL_OUT_5POINT1` = FL | FR | FC | LFE | SL | SR
- `AUDIO_CHANNEL_OUT_7POINT1` = FL | FR | FC | LFE | SL | SR | BL | BR

### TV Physical Output (2.1 Target)
- Left (L)
- Right (R) 
- Subwoofer/LFE

**Downmix Matrix Required** (conceptual):
```
L_out = L + α*C + β*SL + γ*SR + δ*LFE
R_out = R + α*C + β*SR + γ*SL + δ*LFE
LFE_out = ε*LFE + ζ*(L+R)  (bass management)
```

Where coefficients come from persistent properties.

## Format-Specific Handling

### AC-3 (audio/ac3)
- Extractor: MTK AC3Extractor → audio/ac3
- Decoder: OMX.google.ac3.decoder → "ac3dec" → FFmpeg AC3 decoder
- Max channels: 6 (5.1)
- Sample rates: 32, 44.1, 48 kHz

### E-AC-3 (audio/eac3, audio/eac3-joc)
- Extractor: MTK AC3Extractor (same, detects E-AC-3) → audio/eac3 or audio/eac3-joc
- Decoder: OMX.google.eac3.decoder → "eac3dec" → FFmpeg EAC3 decoder
- Max channels: 8 (7.1) or 16 (with Atmos/JOC)
- Sample rates: 32, 44.1, 48 kHz
- **Atmos/JOC**: Metadata present but decoder outputs PCM only (metadata discarded)

### DTS (audio/vnd.dts)
- Extractor: MTK DTSExtractor → audio/vnd.dts
- Decoder: OMX.google.dts.decoder → "dtsdec" → FFmpeg DCA decoder
- Max channels: 8
- Sample rates: 8-192 kHz

### DTS-HD / DTS-ES / DTS-LBR
- Same decoder ("dtsdec") - FFmpeg DCA decoder handles all
- Different MIME types for MediaCodec selection

### TrueHD (audio/true-hd)
- Decoder: OMX.google.truehd.decoder → "truehddec" → FFmpeg TrueHD/MLP decoder
- Special handling in SoftAc3Omx: `processPendingTrueHdBytes()`, major sync detection
- Max channels: 8
- Sample rates: 44.1, 48, 96, 192 kHz

## Verified Code Paths in SoftAc3Omx

From symbol analysis:
```
SoftAc3Omx::initDecoder()
    → avcodec_find_decoder()
    → avcodec_alloc_context3()
    → avcodec_open2()
    → av_parser_init()  (AC3/DTS/MLP parsers)

SoftAc3Omx::onQueueFilled()
    → ingestQueuedInputs()
        → processPendingInputBytes()
            → decodePacket()
                → avcodec_send_packet()
                → drainAvailableFrames()
                    → avcodec_receive_frame()
                    → enqueueDecodedFrame()
                        → ensureResampler()  [swr_init for format conversion]
                        → flushResamplerToPcm()

SoftAc3Omx::processPendingTrueHdBytes()  [TrueHD-specific]
    → Major sync detection
    → Resync logic
```

## Questions Requiring Runtime Verification

1. **Does AudioTrack accept multichannel PCM?** Need to test with 5.1 AudioTrack
2. **Where does downmix actually occur?** AudioFlinger mixer? HAL? Decoder?
3. **What PCM format does decoder output?** Float planar? Int16 interleaved?
4. **Is hardware decode ever preferred?** Check OMX.MS.* decoders in media_codecs.xml
5. **TrueHD path working?** Special sync logic suggests it may be fragile
6. **Atmos metadata?** Discarded in PCM conversion - verify

## Test Plan for Path Verification

```
Test 1: AC3 5.1 → Verify software decoder selected → Check PCM output format
Test 2: E-AC-3 5.1 → Same
Test 3: E-AC-3 7.1 → Same
Test 4: DTS 5.1 → Same  
Test 5: TrueHD → Verify special sync handling
Test 6: Channel mapping test → Unique tone per channel → Measure TV output
Test 7: CPU usage measurement during decode
Test 8: Latency measurement (decode + render)
```
# Phase 4: Old Decoder Implementation Reconstruction

## How AC-3/E-AC-3/DTS Became Visible to Android

### 1. MediaCodec Declaration (media_codecs.xml)
```xml
<MediaCodec name="OMX.google.ac3.decoder">
    <Type name="audio/ac3">
        <Limit name="channel-count" max="6" />
        <Limit name="sample-rate" ranges="32000,44100,48000" />
        <Limit name="bitrate" range="32000-640000" />
    </Type>
</MediaCodec>
<MediaCodec name="OMX.google.eac3.decoder">
    <Type name="audio/eac3">
        <Limit name="channel-count" max="8" />
        <Limit name="sample-rate" ranges="32000,44100,48000" />
        <Limit name="bitrate" range="32000-6144000" />
    </Type>
    <Type name="audio/eac3-joc">
        <Limit name="channel-count" max="16" />
        <Limit name="sample-rate" ranges="32000,44100,48000" />
        <Limit name="bitrate" range="32000-6144000" />
    </Type>
</MediaCodec>
<MediaCodec name="OMX.google.dts.decoder" type="audio/vnd.dts">...</MediaCodec>
<MediaCodec name="OMX.google.dtshd.decoder" type="audio/vnd.dts.hd">...</MediaCodec>
<MediaCodec name="OMX.google.dtse.decoder" type="audio/vnd.dts.hd;profile=lbr">...</MediaCodec>
<MediaCodec name="OMX.google.dtslbr.dec" type="audio/vnd.dts.lbr">...</MediaCodec>
<MediaCodec name="OMX.google.truehd.decoder" type="audio/true-hd">...</MediaCodec>
```

### 2. SoftOMXPlugin Registration (libstagefright_omx.so)
The `SoftOMXPlugin` class in `libstagefright_omx.so` registers these components:

```cpp
// From symbol analysis:
OMX.google.ac3.decoder        → "ac3dec"
OMX.google.eac3.decoder       → "eac3dec"  
OMX.google.dts.decoder        → "dtsdec"
OMX.google.dtshd.decoder      → "dtsdec"
OMX.google.dtse.decoder       → "dtsdec"
OMX.google.dtslbr.dec         → "dtsdec"
OMX.google.truehd.decoder     → "truehddec"
```

All map to **the same unified decoder library** with different component names.

### 3. Component Factory (createSoftOMXComponent)
```cpp
// In libstagefright_omx.so:
createSoftOMXComponent(name, callbacks, appData, component)
    → if (strcmp(name, "ac3dec") == 0) 
           return new SoftAc3Omx(...);  // Actually creates SoftAc3Omx for ALL formats
      else if (strcmp(name, "eac3dec") == 0)
           return new SoftAc3Omx(...);
      // ... etc for dtsdec, truehddec
```

### 4. Unified Decoder: SoftAc3Omx (libstagefright_soft_ac3dec.so)

**Class Hierarchy:**
```
SoftAc3Omx
    → SimpleSoftOMXComponent
        → SoftOMXComponent
            → OMXComponent (interface)
```

**Key Methods:**
```cpp
// Initialization
SoftAc3Omx::initDecoder()
    - avcodec_find_decoder(AV_CODEC_ID_AC3/EAC3/DCA/TRUEHD)
    - avcodec_alloc_context3()
    - avcodec_open2()
    - av_parser_init(AV_CODEC_ID_AC3/DCA/MLP)

// Decode loop
SoftAc3Omx::onQueueFilled()
    → ingestQueuedInputs()
        → processPendingInputBytes()
            → decodePacket()
                → avcodec_send_packet()
                → drainAvailableFrames()
                    → avcodec_receive_frame()
                    → enqueueDecodedFrame()
                        → ensureResampler()  // swr_init for format conversion
                        → flushResamplerToPcm()

// TrueHD special handling
SoftAc3Omx::processPendingTrueHdBytes()
    - Major sync frame detection (0xF8726FBA)
    - Resync logic for corrupted streams
    - Epoch-based packet tracking

// Cleanup
SoftAc3Omx::releaseDecoder()
    - avcodec_flush_buffers()
    - avcodec_free_context()
    - av_channel_layout_uninit()
```

### 5. FFmpeg Integration (libavcodec.so)

**Custom Build Configuration:**
```bash
--enable-decoder='ac3,eac3,dca,truehd,mlp'
--enable-parser='ac3,dca,mlp'
--disable-everything
--enable-avcodec --enable-avutil --enable-swresample
--target-os=android --arch=arm --cpu=armv7-a
```

**Decoder Capabilities:**
| Codec | FFmpeg ID | Parser | Max Channels |
|-------|-----------|--------|--------------|
| AC-3 | AV_CODEC_ID_AC3 | AV_CODEC_ID_AC3 | 6 (5.1) |
| E-AC-3 | AV_CODEC_ID_EAC3 | AV_CODEC_ID_AC3 | 8 (7.1) / 16 (JOC) |
| DTS | AV_CODEC_ID_DCA | AV_CODEC_ID_DCA | 8 |
| DTS-HD | AV_CODEC_ID_DCA | AV_CODEC_ID_DCA | 8 (core only, no XLL) |
| TrueHD | AV_CODEC_ID_TRUEHD | AV_CODEC_ID_MLP | 8 |
| MLP | AV_CODEC_ID_MLP | AV_CODEC_ID_MLP | 8 |

**Limitations:**
- **DTS-HD**: Only decodes core (lossy) layer, not XLL lossless
- **TrueHD**: Decodes MLP core, metadata handling minimal
- **Atmos/JOC**: Metadata present in E-AC-3 but **discarded** - PCM output only
- **No Dolby Vision audio**, no DTS:X

### 6. Buffer/Format Expectations

**Input (Compressed):**
- OMX buffer: Compressed frames (AC3/E-AC3/DTS/TrueHD)
- Multiple frames per buffer possible
- Parser splits into individual frames

**Output (PCM):**
- Format: Determined by `ensureResampler()` / `swr_init`
- Likely: 16-bit interleaved or float planar
- Sample rate: Stream native (resampled if needed)
- Channel layout: Stream native (converted if needed)

**Buffer Management:**
- Input port: Multiple buffers, client allocates
- Output port: Multiple buffers, component allocates
- PCM queue: Internal queue between decoder and resampler

## What Was Actually Modified vs Stock Android 9

### Stock Android 9 (AOSP) Has:
- `libstagefright_soft_ac3dec.cpp` - Basic AC3 decoder using FFmpeg
- `libstagefright_soft_eac3dec.cpp` - Basic E-AC3 decoder  
- `libstagefright_soft_dtsdec.cpp` - Basic DTS decoder
- Separate libraries for each format

### This Implementation Changed:
1. **Merged all 4 decoders into ONE library** (libstagefright_soft_ac3dec.so)
2. **Single SoftAc3Omx class handles ALL formats** via runtime codec selection
3. **Custom FFmpeg build** (8.1 vs stock ~4.x) with only needed decoders
4. **TrueHD special sync handling** added
5. **Unified parser initialization** (AC3 parser used for E-AC-3 too)

### What Was NOT Modified (Stock Behavior):
- MediaExtractor (MTK extractors are stock MTK)
- MediaCodecList / MediaCodec selection logic
- AudioFlinger mixer
- AudioPolicyService
- AudioTrack API
- OMX framework (SoftOMXPlugin is stock pattern)

## ABI/API Compatibility Requirements

### Android 9 (API 28) / VNDK 28
- Target: armv7-a (32-bit)
- NDK: r21+ compatible
- VNDK: libstagefright, libstagefright_omx, libmedia_omx, libavcodec, libavutil, libswresample

### Required Symbols (from libstagefright_omx.so)
```
createSoftOMXComponent
SimpleSoftOMXComponent::addPort
SimpleSoftOMXComponent::internalGetParameter
SimpleSoftOMXComponent::internalSetParameter
SoftOMXComponent::getExtensionIndex
SoftOMXComponent::notifyFillBufferDone
SoftOMXComponent::notifyEmptyBufferDone
```

### Required Symbols (from libstagefright_foundation.so)
```
AMessage, AHandler, ALooper
MediaSource, MetaData
OMX_BUFFERHEADERTYPE, OMX_PARAM_PORTDEFINITIONTYPE
```

### Required Symbols (from libavcodec.so)
```
avcodec_find_decoder
avcodec_alloc_context3
avcodec_open2
av_parser_init
avcodec_send_packet
avcodec_receive_frame
avcodec_flush_buffers
avcodec_free_context
av_channel_layout_uninit
```

### Required Symbols (from libswresample.so)
```
swr_init
swr_convert
swr_free
```

## Replacement Strategy

**DO NOT** replicate the "identical library copies" approach.

**INSTEAD** build a single unified decoder library with:
1. **Plugin architecture** - One .so, multiple OMX component names
2. **Clean FFmpeg wrapper** - Abstract codec-specific logic
3. **Proper PCM normalization layer** - Separate from decoder
4. **Configurable downmix** - Not hardcoded in decoder

The old implementation proves: **A single unified decoder works**. We just need to rebuild it cleanly.
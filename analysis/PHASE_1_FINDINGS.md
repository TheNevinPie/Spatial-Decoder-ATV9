# Phase 1: Forensic Extraction - Findings

## Extracted File Inventory

### System Libraries (`/system/lib/`)
| File | Size | SHA256 | Notes |
|------|------|--------|-------|
| libavcodec.so | 446,252 | custom | **Custom FFmpeg 8.1 build** - enables only ac3,eac3,dca,truehd,mlp decoders |
| libavutil.so | 430,176 | custom | FFmpeg utility library |
| libswresample.so | 66,496 | custom | FFmpeg resampling library |
| libstagefright.so | 1,008,236 | stock | Core Stagefright framework |
| libstagefright_omx.so | 294,140 | modified | **SoftOMXPlugin registers unified decoder** |
| libstagefright_soft_ac3dec.so | 58,276 | modified | **Unified decoder for AC3/EAC3/DTS/DTS-HD/TrueHD** |
| libstagefright_soft_eac3dec.so | 58,276 | IDENTICAL | Hard link / copy of ac3dec |
| libstagefright_soft_dtsdec.so | 58,276 | IDENTICAL | Hard link / copy of ac3dec |
| libstagefright_soft_truehddec.so | 58,276 | IDENTICAL | Hard link / copy of ac3dec |
| libmedia_omx.so | 187,336 | stock | OMX media utilities |

**KEY FINDING**: All four "soft decoder" libraries are **IDENTICAL** (SHA256: `815a8229903843aba52515af03f0a50caa67a2938f16e826622375dd9e1ca6b8`). This is a single unified decoder library that registers multiple OMX component names.

### Vendor Libraries (`/vendor/lib/`)
| File | Size | Notes |
|------|------|-------|
| libaudioparser.so | 69,160 | Audio parsing utilities |
| libeffects.so | 19,668 | Audio effects framework |
| libeffectsconfig.so | 19,236 | Effects configuration |
| libwebrtc_audio_preprocessing.so | 813,324 | WebRTC audio processing |

### Vendor HW (`/vendor/lib/hw/`)
| File | Size | SHA256 | Notes |
|------|------|--------|-------|
| audio.primary.mt5862.so | 192,296 | modified 2026-06-18 | **Primary Audio HAL** - supports HW decode + SW decode paths |
| audio.voiceprocess.mt5862.so | 33,120 | | Voice processing |
| android.hardware.audio@4.0-impl.so | 199,392 | | Audio HAL HIDL impl |
| android.hardware.audio.effect@4.0-impl.so | 209,452 | | Audio effects HAL |

### Vendor Config (`/vendor/etc/`)
| File | Notes |
|------|-------|
| media_codecs.xml | **Declares OMX.google.* software decoders** for AC3, EAC3, DTS, DTS-HD, DTS-ES, DTS-LBR, TrueHD |
| media_codecs_google_audio.xml | Standard Google audio codecs (MP3, AAC, Vorbis, Opus, FLAC, etc.) |
| media_codecs_google_video_le.xml | Standard Google video codecs |
| audio_policy_configuration.xml | **Primary output: Stereo only. Offload output: Compressed passthrough** |
| audio_effects.xml | **Includes libdownmix.so + libjamesdsp.so** |
| audio_policy.conf | Legacy audio policy |

### Media Extractors (`/system/lib/extractors/`)
| File | Notes |
|------|-------|
| libac3.mtk.so | **MTK AC3/E-AC3 Extractor** - detects both formats, outputs audio/ac3 or audio/eac3 |
| libdts.mtk.so | **MTK DTS Extractor** - detects DTS, outputs audio/vnd.dts |

### Magisk Modules (`/data/adb/modules/`)
| Module | Purpose |
|--------|---------|
| SystemBypass | Replaces services.jar (framework modification) |
| YetAnotherBootloopProtector | Boot protection |
| logger | Logging module |

### System Properties (Persistent)
```
persist.vendor.pie.5_1.center=0.90
persist.vendor.pie.5_1.lfe=0.25
persist.vendor.pie.5_1.surround=0.55
persist.vendor.pie.5_1.width=0.95
persist.vendor.pie.7_1.center=0.760
persist.vendor.pie.7_1.lfe=0.030
persist.vendor.pie.7_1.rear=0.620
persist.vendor.pie.7_1.side=0.780
persist.vendor.pie.7_1.width=1.03
persist.vendor.pie.atmos.matrix_encoding=dplii
persist.vendor.pie.surround.matrix_encoding=dplii
```

---

## Architecture Analysis

### Codec Registration Path
```
Application
    → MediaExtractor (MTK AC3/DTS Extractor detects stream)
    → MIME type: audio/ac3, audio/eac3, audio/vnd.dts, etc.
    → MediaCodecList (via media_codecs.xml)
    → OMX.google.ac3.decoder / OMX.google.eac3.decoder / OMX.google.dts.decoder / etc.
    → SoftOMXPlugin (libstagefright_omx.so)
    → createSoftOMXComponent("ac3dec") / "eac3dec" / "dtsdec" / "truehddec"
    → libstagefright_soft_ac3dec.so (UNIFIED DECODER)
    → SoftAc3Omx class
    → FFmpeg libavcodec (avcodec_find_decoder, avcodec_send_packet, avcodec_receive_frame)
    → PCM output
    → libswresample (swr_init) for resampling/channel conversion
    → AudioTrack / AudioFlinger
    → Audio Policy (audio_policy_configuration.xml)
    → Audio HAL (audio.primary.mt5862.so)
    → Physical Speaker (2.0 stereo per policy)
```

### Unified Decoder Implementation (SoftAc3Omx)
The single library `libstagefright_soft_ac3dec.so` implements:
- **SoftAc3Omx** class handling ALL formats: AC3, E-AC3, DTS, DTS-HD, DTS-ES, DTS-LBR, TrueHD
- Uses **FFmpeg libavcodec** via dynamic linking (avcodec_find_decoder, avcodec_alloc_context3, avcodec_open2, av_parser_init, avcodec_send_packet, avcodec_receive_frame, avcodec_flush_buffers, avcodec_free_context)
- Uses **libswresample** for resampling/channel layout conversion (swr_init)
- Has **TrueHD-specific logic** (processPendingTrueHdBytes, major sync detection, resync)
- Registers **7 OMX component roles** via SoftOMXPlugin:
  - OMX.google.ac3.decoder → "ac3dec"
  - OMX.google.eac3.decoder → "eac3dec"
  - OMX.google.dts.decoder → "dtsdec"
  - OMX.google.dtshd.decoder → "dtsdec"
  - OMX.google.dtse.decoder → "dtsdec"
  - OMX.google.dtslbr.dec → "dtsdec"
  - OMX.google.truehd.decoder → "truehddec"

### Custom FFmpeg Build (libavcodec.so)
- **Version**: FFmpeg 8.1
- **Build path**: `C:/tv_projects/ffmpeg_8_1_src/`
- **Configuration**: 
  ```
  --enable-decoder='ac3,eac3,dca,truehd,mlp'
  --enable-parser='ac3,dca,mlp'
  --disable-everything
  --enable-avcodec --enable-avutil --enable-swresample
  --disable-programs --disable-doc --disable-network --disable-iconv
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale
  --enable-small --extra-cflags=-fPIC --extra-ldflags=-fPIC
  ```
- **Target**: Android ARMv7 (32-bit)

### Audio Policy Configuration
**Primary Output (PCM)**:
- Format: PCM 16-bit, 48kHz, Stereo only
- This is the **only** PCM output path to speakers

**Offload Output (Compressed Passthrough)**:
- Supports: AC3, E-AC3, DTS, DTS-HD, AAC, AC4
- Channel masks: Mono through 7.1
- Routes to: Speaker, Headset, HDMI, SPDIF, BT A2DP, USB
- **Used for hardware passthrough**, not software decode

### Downmix Path
**Current State**:
1. `audio_effects.xml` declares `libdownmix.so` (standard Android downmix) and `libjamesdsp.so` (JamesDSP)
2. Persistent properties define downmix coefficients for 5.1 and 7.1
3. Audio HAL has "soft" string reference but unclear if software downmix is active
4. Primary output is stereo-only per policy - **no multichannel PCM output configured**

**Property-to-Coefficient Mapping** (from old implementation):
| Property | 5.1 Default | 7.1 Default | Purpose |
|----------|-------------|-------------|---------|
| persist.vendor.pie.5_1.center | 0.90 | - | Center channel gain |
| persist.vendor.pie.5_1.surround | 0.55 | - | Surround channel gain |
| persist.vendor.pie.5_1.lfe | 0.25 | - | LFE gain |
| persist.vendor.pie.5_1.width | 0.95 | - | Stereo width |
| persist.vendor.pie.7_1.center | - | 0.760 | Center gain |
| persist.vendor.pie.7_1.side | - | 0.780 | Side surround gain |
| persist.vendor.pie.7_1.rear | - | 0.620 | Rear surround gain |
| persist.vendor.pie.7_1.lfe | - | 0.030 | LFE gain |
| persist.vendor.pie.7_1.width | - | 1.03 | Stereo width |
| persist.vendor.pie.atmos.matrix_encoding | dplii | - | Atmos matrix mode |
| persist.vendor.pie.surround.matrix_encoding | dplii | - | Surround matrix mode |

---

## Dependency Graph

```
libstagefright_soft_ac3dec.so (unified decoder)
    ├── libavcodec.so (FFmpeg 8.1 - custom build)
    │   └── libavutil.so
    ├── libswresample.so
    ├── libstagefright_omx.so (SoftOMXPlugin base)
    ├── libstagefright_foundation.so
    └── libstagefright.so

libstagefright_omx.so
    ├── libmedia_omx.so
    ├── libstagefright_foundation.so
    ├── libstagefright_bufferqueue_helper.so
    ├── libstagefright_xmlparser.so
    └── android.hardware.media.omx@1.0.so

audio.primary.mt5862.so (Audio HAL)
    ├── libaudioparser.so
    ├── MI_SYS / MI_AUDIO / MI_ACAP / MI_AOUT (MediaTek proprietary)
    └── android.hardware.audio@4.0-impl.so

Media Extractors:
    libac3.mtk.so → libstagefright.so, libstagefright_foundation.so
    libdts.mtk.so → libstagefright.so, libstagefright_foundation.so
```

---

## Minimum Working Set for Software Decode Pipeline

### REQUIRED (Core Functionality)
1. **Custom libavcodec.so + libavutil.so + libswresample.so** - FFmpeg decoders
2. **Unified decoder library** (libstagefright_soft_ac3dec.so) - OMX component wrapper
3. **libstagefright_omx.so** - SoftOMXPlugin registration
4. **media_codecs.xml** - Codec declarations for OMX.google.* decoders
5. **MTK Extractors** (libac3.mtk.so, libdts.mtk.so) - MIME detection

### REQUIRED (Audio Output)
6. **audio_policy_configuration.xml** - Must add multichannel PCM support to primary output
7. **Audio HAL modifications** - To accept multichannel PCM and downmix to 2.1

### OPTIONAL (Enhancement)
8. **Downmix configuration** - Persistent properties or config file
9. **High-quality downmix library** - Replacement for libdownmix.so
10. **JamesDSP** - Equalizer/effects (separate concern)

### WORKAROUND / FRAGILE (Current Implementation)
11. **services.jar replacement** (SystemBypass module) - Framework modification, purpose unclear
12. **Identical decoder library copies** - Wasteful, should use single library with multiple entry points
13. **VNDK duplicates** - Same libraries in /system/lib/vndk-28/

### UNRELATED (Should Not Be Reproduced)
- CVTE debloat scripts (service.d/)
- Boot video modifications
- Bluetooth removal
- SystemUI component disabling
- JamesDSP installation (separate audio effects project)
- YABP (bootloop protector)

---

## Execution Flow Verification Needed

1. [ ] Trace actual codec selection when playing AC3/E-AC3/DTS content
2. [ ] Verify software decoder is selected over hardware decoder
3. [ ] Confirm PCM format output from decoder (sample rate, channel layout, bit depth)
4. [ ] Verify downmix actually occurs (where? AudioTrack? AudioFlinger? HAL?)
5. [ ] Test with 5.1 and 7.1 content - verify channel mapping
6. [ ] Measure CPU usage during decode
7. [ ] Verify TrueHD path (separate decoder or same?)
8. [ ] Check if Atmos metadata is preserved or discarded
# Implementation Plan - Candidate A (Proven Path)

## Architecture Summary
```
Compressed Audio
    → MTK Extractor (MIME detection)
    → MediaCodec (OMX.google.*.decoder)
    → SoftOMXPlugin → createSoftOMXComponent("ac3dec"/"eac3dec"/"dtsdec"/"truehddec")
    → libspatialdecoder.so (NEW unified decoder)
        ├── FFmpeg Backend (decode → multichannel float planar)
        ├── PCM Normalization (resample, layout, format)
        └── spatialdownmix (5.1/7.1 → stereo float)
            ├── Configurable matrix (from properties + config file)
            ├── Bass management (LFE + redirected bass)
            ├── Peak limiter / soft clip
            └── Headroom management
    → convertFloatPcmToS16
    → AudioTrack (STEREO) → AudioFlinger → HAL → Speakers
```

---

## Phase 1: Decoder Backend (libspatialdecoder.so)

### 1.1 FFmpeg Backend
- **Build**: FFmpeg 8.1, same configure flags as original
  ```
  --enable-decoder=ac3,eac3,dca,truehd,mlp
  --enable-parser=ac3,dca,mlp
  --disable-everything
  --enable-avcodec --enable-avutil --enable-swresample
  --target-os=android --arch=arm --cpu=armv7-a
  --enable-shared --disable-static
  --extra-cflags=-fPIC --extra-ldflags=-fPIC
  ```
- **API**: `avcodec_find_decoder`, `avcodec_alloc_context3`, `avcodec_open2`, `av_parser_init`, `avcodec_send_packet`, `avcodec_receive_frame`, `avcodec_flush_buffers`, `avcodec_free_context`, `av_channel_layout_copy`, `av_channel_layout_uninit`

### 1.2 OMX Component Plugin
- Export: `createSoftOMXComponent(name, callbacks, appData, component)`
- Map internal name → codec:
  - "ac3dec" → AV_CODEC_ID_AC3
  - "eac3dec" → AV_CODEC_ID_EAC3
  - "dtsdec" → AV_CODEC_ID_DCA
  - "truehddec" → AV_CODEC_ID_TRUEHD
- Implement OMX callbacks: GetParameter, SetParameter, EmptyThisBuffer, FillThisBuffer, GetState, etc.

### 1.3 Build Output
- `libspatialdecoder.so` (ARMv7, Android 9, API 28)
- Place in: `/vendor/lib/` (replaces 4 decoder libraries)

---

## Phase 2: PCM Normalization Layer

### 2.1 Resampler (libswresample wrapper)
- Input: FFmpeg output (float planar, multichannel, stream sample rate)
- Output: Stereo float planar, 48000 Hz (or stream rate if no resample needed)
- High-quality mode (swr_setopts: swr_flags=accurate_rnd)

### 2.2 Channel Layout Normalization
- Map FFmpeg layouts → internal standard
- Ensure consistent channel ordering before downmix

### 2.3 Format Conversion
- Float planar → S16 interleaved (for AudioTrack)
- Headroom: -6dBFS before limiter

---

## Phase 3: Downmix Engine (libspatialdownmix.so)

### 3.1 Configurable Matrix
```c
struct DownmixConfig {
    // 5.1 coefficients
    float center_5_1;
    float surround_5_1;
    float lfe_5_1;
    float width_5_1;
    
    // 7.1 coefficients
    float center_7_1;
    float side_7_1;
    float rear_7_1;
    float lfe_7_1;
    float width_7_1;
    
    // Matrix encoding
    enum { DPLII, STEREO } matrix_5_1, matrix_7_1, matrix_atmos;
    
    // Limiter
    float trim_target_db;
    float release_ms;
    bool lfe_lowpass_enabled;
    float lfe_crossover_hz;
};
```

### 3.2 Processing
```
Input: 5.1/7.1 float planar (or stereo passthrough)
Output: Stereo float planar

L_out = FL + c*FC + s*(SL+SR) + lfe*LFE  [5.1]
R_out = FR + c*FC + s*(SR+SL) + lfe*LFE

L_out = FL + c*FC + s*SL + r*BL + lfe*LFE  [7.1]
R_out = FR + c*FC + s*SR + r*BR + lfe*LFE

LFE_out = LFE + bass_redirect(FL+FR)  [optional bass management]
```

### 3.3 Peak Limiter
- Look-ahead peak detection
- Soft knee compression above trim_target
- Release time configurable
- Soft clipping as final safety

### 3.4 Configuration Sources (priority order)
1. `/vendor/etc/spatialdecoder_config.json` (file)
2. `persist.vendor.pie.*` properties (compat)
3. Hardcoded defaults

---

## Phase 4: Integration & Installation

### 4.1 Files to Install
```
/vendor/lib/libspatialdecoder.so           # Unified decoder + PCM + downmix
/vendor/etc/spatialdecoder_config.json     # Configuration
```

### 4.2 Files to Replace (Backup First)
```
/system/lib/libstagefright_soft_ac3dec.so       → libspatialdecoder.so
/system/lib/libstagefright_soft_eac3dec.so      → libspatialdecoder.so
/system/lib/libstagefright_soft_dtsdec.so       → libspatialdecoder.so
/system/lib/libstagefright_soft_truehddec.so    → libspatialdecoder.so
/system/lib/vndk-28/libstagefright_soft_ac3dec.so   → libspatialdecoder.so
/system/lib/vndk-28/libstagefright_soft_eac3dec.so  → libspatialdecoder.so
/system/lib/vndk-28/libstagefright_soft_dtsdec.so   → libspatialdecoder.so
/system/lib/vndk-28/libstagefright_soft_truehddec.so→ libspatialdecoder.so
```

### 4.3 media_codecs.xml (NO CHANGE - keeps OMX.google.*)
```xml
<MediaCodec name="OMX.google.ac3.decoder">...</MediaCodec>
<MediaCodec name="OMX.google.eac3.decoder">...</MediaCodec>
<MediaCodec name="OMX.google.dts.decoder">...</MediaCodec>
<MediaCodec name="OMX.google.dtshd.decoder">...</MediaCodec>
<MediaCodec name="OMX.google.dtse.decoder">...</MediaCodec>
<MediaCodec name="OMX.google.dtslbr.dec">...</MediaCodec>
<MediaCodec name="OMX.google.truehd.decoder">...</MediaCodec>
```

### 4.4 Install Script
```bash
#!/system/bin/sh
# backup.sh - Run first
# install.sh - Replace libraries
# verify.sh - Test all formats
# restore.sh - Rollback
```

### 4.5 Verification
```bash
# 1. Codec registration
dumpsys media.codec | grep -E "ac3|eac3|dts|truehd"

# 2. Playback test each format
# 3. Channel mapping test (unique tones)
# 4. Property override test
# 5. CPU usage measurement
```

---

## Build System

### Android.bp (Soong)
```bp
cc_library_shared {
    name: "libspatialdecoder",
    srcs: ["src/decoder/*.cpp", "src/pcm/*.cpp", "src/downmix/*.cpp"],
    shared_libs: ["libavcodec", "libavutil", "libswresample", "libstagefright_omx", "libstagefright_foundation"],
    cflags: ["-std=c++17", "-fPIC", "-O2"],
    target: { android: { cflags: ["-DANDROID"] } },
}

cc_library_shared {
    name: "libspatialdownmix",
    srcs: ["src/downmix/*.cpp"],
    cflags: ["-std=c++17", "-fPIC", "-O2"],
}
```

### Toolchain
- NDK r21+ (API 28)
- ARMv7-A (32-bit)
- Android 9 (PPR2.180905.006.A1)

---

## Testing Strategy

### Unit Tests (Host)
- Downmix engine: 5.1 WAV → stereo WAV, compare coefficients
- Resampler: various rates → 48kHz
- Limiter: full-scale input → no clipping

### Integration Tests (Target)
1. **Channel ID Test**: Play test_ac3_51_chid.ac3 → verify L/R/LFE tones
2. **Format Test**: Play each test file → verify playback
3. **Property Test**: Change center=0.0 → verify center muted
4. **CPU Test**: `top -H -p <mediaserver>` during playback
5. **Passthrough Test**: HDMI/SPDIF compressed passthrough still works

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Bootloop | backup.sh + restore.sh, test in recovery |
| SELinux | Include .te file, restorecon after install |
| Codec not found | verify.sh checks dumpsys media.codec |
| Wrong downmix | Channel ID test tones |
| Breaks passthrough | Offload path unchanged in policy |
| Performance | Benchmark, optimize buffer sizes |

---

## Milestones

| Milestone | Deliverable | Criteria |
|-----------|-------------|----------|
| M1 | libspatialdecoder.so builds | Compiles, exports createSoftOMXComponent |
| M2 | OMX registration works | dumpsys shows OMX.google.* registered |
| M3 | AC3 5.1 plays | Audio output, channel mapping correct |
| M4 | All formats play | AC3, E-AC3, DTS, TrueHD |
| M5 | Downmix matches golden | Coefficients within ±0.001 |
| M6 | Property override works | Runtime property changes affect output |
| M7 | Install/reverse verified | restore.sh returns to stock |
| M8 | Performance acceptable | CPU < 25%, no underruns |

---

## Next Step: Start M1 - Build libspatialdecoder.so
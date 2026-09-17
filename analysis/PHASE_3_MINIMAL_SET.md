# Phase 3: Minimum Working Set Classification

## Classification of All Old Modifications

| Component | Classification | Reason |
|-----------|----------------|--------|
| **libavcodec.so (custom FFmpeg 8.1)** | **REQUIRED** | Core decoder - only way to decode AC3/E-AC3/DTS/TrueHD in software |
| **libavutil.so** | **REQUIRED** | FFmpeg dependency |
| **libswresample.so** | **REQUIRED** | Resampling/channel conversion |
| **libstagefright_soft_ac3dec.so (unified)** | **REQUIRED** | OMX component wrapper for all formats |
| **libstagefright_soft_eac3dec.so** | **REDUNDANT** | Identical copy - remove |
| **libstagefright_soft_dtsdec.so** | **REDUNDANT** | Identical copy - remove |
| **libstagefright_soft_truehddec.so** | **REDUNDANT** | Identical copy - remove |
| **libstagefright_omx.so (modified SoftOMXPlugin)** | **REQUIRED** | Registers unified decoder components |
| **media_codecs.xml (vendor)** | **REQUIRED** | Declares OMX.google.* software decoders |
| **libac3.mtk.so (extractor)** | **REQUIRED** | MIME detection for AC3/E-AC3 |
| **libdts.mtk.so (extractor)** | **REQUIRED** | MIME detection for DTS variants |
| **audio_policy_configuration.xml** | **REQUIRED (MODIFIED)** | Must add multichannel PCM to primary output |
| **audio_effects.xml** | **OPTIONAL** | Downmix effect - replace with custom |
| **persist.vendor.pie.* properties** | **OPTIONAL** | Configuration for downmix - formalize |
| **audio.primary.mt5862.so** | **WORKAROUND** | Current HAL may not support multichannel PCM input |
| **services.jar (SystemBypass module)** | **UNRELATED** | Framework modification - purpose unclear, likely not needed |
| **JamesDSP (libjamesdsp.so)** | **UNRELATED** | Separate audio effects project |
| **VNDK duplicates (/system/lib/vndk-28/*)** | **REDUNDANT** | Same libs duplicated - not needed |
| **Magisk modules (YABP, logger)** | **UNRELATED** | Boot protection/logging - separate |
| **Debloat scripts (service.d/)** | **UNRELATED** | System cleanup - separate project |
| **Boot video modifications** | **UNRELATED** | Unrelated to audio |

## Minimum Working Set (Clean Implementation)

### 1. Decoder Backend (New Build)
```
src/decoder/
├── ffmpeg_decoder.{h,cpp}      # FFmpeg wrapper (avcodec + swresample)
├── ac3_decoder.{h,cpp}         # AC-3 specific
├── eac3_decoder.{h,cpp}        # E-AC-3 specific  
├── dts_decoder.{h,cpp}         # DTS/DTS-HD/DTS-ES/DTS-LBR
├── truehd_decoder.{h,cpp}      # TrueHD/MLP
└── decoder_factory.{h,cpp}     # Format → decoder mapping
```

### 2. PCM Normalization Layer
```
src/pcm/
├── pcm_format.{h,cpp}          # Internal format definition
├── sample_converter.{h,cpp}    # Format conversion (float↔int16, planar↔interleaved)
├── channel_layout.{h,cpp}      # Layout normalization, mapping
├── resampler.{h,cpp}           # High-quality resampling (swr wrapper)
└── pcm_buffer.{h,cpp}          # Buffer management
```

### 3. Downmix Engine (New - Audiophile Quality)
```
src/downmix/
├── downmix_engine.{h,cpp}      # Core downmix processing
├── downmix_matrix.{h,cpp}      # Coefficient matrices
├── bass_management.{h,cpp}     # LFE handling, crossover
├── gain_control.{h,cpp}        # Headroom, limiter, soft clip
├── config.{h,cpp}              # Configuration system
└── presets.{h,cpp}             # Standard presets (ITU, Dolby, custom)
```

### 4. Android Integration Layer
```
src/android/
├── omx_component.{h,cpp}       # OMX component implementation
├── soft_omx_plugin.{h,cpp}     # SoftOMXPlugin replacement
├── media_codec_registry.{h,cpp} # Codec registration
├── audio_track_sink.{h,cpp}    # AudioTrack output
├── audio_policy_helper.{h,cpp} # Policy interaction
└── hal_interface.{h,cpp}       # Audio HAL communication
```

### 5. Configuration System
```
src/config/
├── config_parser.{h,cpp}       # Parse config file/properties
├── property_bridge.{h,cpp}     # Android system properties
└── config_schema.json          # Validation schema
```

### 6. Build System
```
build/
├── Android.bp                  # Soong build (preferred)
├── CMakeLists.txt              # CMake alternative
├── toolchain.cmake             # Android NDK toolchain
└── modules/
    ├── decoder/
    ├── pcm/
    ├── downmix/
    └── android/
```

### 7. Installation/Recovery
```
flash/
├── install.sh                  # Main installer
├── uninstall.sh                # Clean removal
├── backup.sh                   # Backup original files
├── restore.sh                  # Restore from backup
├── verify.sh                   # Post-install verification
├── manifest.json               # Installation manifest
└── sepolicy/
    └── audio_decoder.te        # SELinux policy
```

## Required System Modifications (Minimal)

### 1. media_codecs.xml (vendor/etc/)
**Replace** OMX.google.* decoder entries with new component names:
```xml
<MediaCodec name="OMX.spatialdecoder.ac3" type="audio/ac3">...</MediaCodec>
<MediaCodec name="OMX.spatialdecoder.eac3" type="audio/eac3">...</MediaCodec>
<MediaCodec name="OMX.spatialdecoder.dts" type="audio/vnd.dts">...</MediaCodec>
<MediaCodec name="OMX.spatialdecoder.truehd" type="audio/true-hd">...</MediaCodec>
```

### 2. audio_policy_configuration.xml (vendor/etc/)
**Add** multichannel PCM support to primary output:
```xml
<mixPort name="primary output" role="source" flags="AUDIO_OUTPUT_FLAG_PRIMARY">
    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO" />
    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="48000" channelMasks="AUDIO_CHANNEL_OUT_5POINT1,AUDIO_CHANNEL_OUT_7POINT1" />
    <profile name="" format="AUDIO_FORMAT_PCM_FLOAT" samplingRates="48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO,AUDIO_CHANNEL_OUT_5POINT1,AUDIO_CHANNEL_OUT_7POINT1" />
</mixPort>
```

### 3. New Shared Libraries (install to /vendor/lib/)
- `libspatialdecoder.so` - Main decoder + PCM + downmix
- `libspatialdecoder_omx.so` - OMX component plugin

### 4. Audio HAL Modification (if needed)
**Option A**: Modify audio.primary.mt5862.so to accept multichannel PCM
**Option B**: Use AudioTrack → AudioFlinger downmix (requires policy change above)
**Option C**: Custom HAL module for downmix

**Recommendation**: Option B (least invasive) - let AudioFlinger handle downmix via updated policy + custom downmix effect

## Files to BACKUP Before Installation

| File | Backup Location |
|------|-----------------|
| /vendor/etc/media_codecs.xml | flash/backup/vendor_etc_media_codecs.xml |
| /vendor/etc/audio_policy_configuration.xml | flash/backup/vendor_etc_audio_policy_configuration.xml |
| /vendor/etc/audio_effects.xml | flash/backup/vendor_etc_audio_effects.xml |
| /system/lib/libavcodec.so | flash/backup/system_lib_libavcodec.so |
| /system/lib/libavutil.so | flash/backup/system_lib_libavutil.so |
| /system/lib/libswresample.so | flash/backup/system_lib_libswresample.so |
| /system/lib/libstagefright_soft_ac3dec.so | flash/backup/system_lib_libstagefright_soft_ac3dec.so |
| /system/lib/libstagefright_omx.so | flash/backup/system_lib_libstagefright_omx.so |
| /vendor/lib/hw/audio.primary.mt5862.so | flash/backup/vendor_lib_hw_audio.primary.mt5862.so |

## Risk Register

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Bootloop from media_codecs.xml error | Medium | Critical | Backup + restore script, test in recovery |
| Audio HAL rejects multichannel PCM | High | High | Fallback to stereo decode path |
| SELinux denies new libraries | Medium | High | Include sepolicy module |
| OMX component not registered | Medium | High | Verify with dumpsys media.codec |
| Downmix coefficients wrong | Medium | Medium | Channel mapping test tones |
| CPU overload on decode | Low | Medium | Benchmark, optimize buffer sizes |
| TrueHD sync issues | Medium | Medium | Special handling, fallback to passthrough |
| Breaks hardware passthrough | Low | High | Keep offload path intact |
| VNDK version mismatch | Low | Medium | Build against correct NDK/API level |
# Downmix Location Investigation - CONCLUSIVE FINDINGS

## Summary
**The downmix from 5.1/7.1 → stereo/2.1 happens INSIDE the decoder library (libstagefright_soft_ac3dec.so), NOT in AudioFlinger or the Audio HAL.**

---

## FACT: Direct Binary Evidence

### 1. Decoder Contains Custom Downmix Function
From `libstagefright_soft_ac3dec.so` strings:
```
_ZN7android12_GLOBAL__N_120downmixToStereoFloatEPKfiiRKNS0_13DownmixConfigERKNS0_18BiquadCoefficientsEPNS0_11BiquadStateE
```
Mangled name: `android::(anonymous namespace)::downmixToStereoFloat(float const*, int, int, DownmixConfig const&, BiquadCoefficients const&, BiquadState*)`

### 2. Decoder Trace String Shows Downmix Parameters
```
TRACE %s downmix bucket=%s layout=%s isAtmos=%d profileId=%d matrix=%s c=%.3f s=%.3f r=%.3f lfe=%.3f trimMode=postmix_peak trimTarget=%.3f releaseMs=%.0f lfeLp=%d loudness=strict_reference props=%s,%s,%s,%s,%s,%s,%s,%s,%s
```
Parameters logged during downmix:
- `c` = center gain coefficient
- `s` = surround gain coefficient  
- `r` = rear gain coefficient (7.1)
- `lfe` = LFE gain coefficient
- `matrix` = matrix encoding mode (dplii)
- `trimMode` = postmix_peak (peak limiting after mixing)
- `trimTarget` = target level
- `releaseMs` = release time
- `lfeLp` = LFE low-pass filter enable

### 3. Persistent Properties Read Directly by Decoder
```
persist.vendor.pie.5_1.center
persist.vendor.pie.5_1.surround
persist.vendor.pie.5_1.lfe
persist.vendor.pie.7_1.center
persist.vendor.pie.7_1.lfe
persist.vendor.pie.surround.matrix_encoding
persist.vendor.pie.atmos.matrix_encoding
```

### 4. Decoder Uses swresample for Format Conversion
```
swr_alloc_set_opts2
swr_init
swr_convert
av_channel_layout_copy
av_channel_layout_describe
TRACE %s source channels=%d rate=%d layout=%s fmt=%d
```

---

## FACT: Runtime Evidence

### 1. AudioFlinger Primary Output is STEREO ONLY
```
Output thread 0xae523340, name AudioOut_D
  Channel count: 2
  Channel mask: 0x00000003 (front-left, front-right)
  Processing format: 0x5 (AUDIO_FORMAT_PCM_FLOAT)
  HAL format: 0x1 (AUDIO_FORMAT_PCM_16_BIT)
  0 Effect Chains
```

### 2. Audio HAL Dump Shows downmix_mode = 0
```
Settings:
 - downmix_mode: 0
 - sink capability: DD un-supported, DDP un-supported, DTS un-supported, DTS-HD un-supported, AAC un-supported
```

### 3. No Downmix Effect Chain on Primary Output
```
0 Effect Chains
```
The `libdownmix.so` effect declared in `audio_effects.xml` is NOT instantiated on the primary output thread.

---

## STRONG INFERENCE: Decoder Output Port Configuration

The decoder's OMX output port is configured for STEREO PCM:
- Output channel count: 2 (stereo)
- Output channel layout: FL, FR
- Output format: Float planar (processed) → S16 interleaved (to AudioTrack)

The decoder internally:
1. Decodes multichannel compressed audio → multichannel float PCM (planar)
2. Applies custom downmix (downmixToStereoFloat) → stereo float PCM
3. Uses swresample for any resampling needed
4. Converts float → S16 interleaved (convertFloatPcmToS16)
5. Outputs stereo PCM to AudioTrack

---

## STRONG INFERENCE: Two Distinct Audio Paths

### Path A: Software Decode (Current Working Path)
```
Compressed Audio (AC3/E-AC3/DTS/TrueHD)
    → MTK Extractor (MIME detection)
    → MediaCodec (OMX.google.*.decoder)
    → SoftOMXPlugin → SoftAc3Omx (libstagefright_soft_ac3dec.so)
    → FFmpeg libavcodec (decode)
    → Internal downmixToStereoFloat (5.1/7.1 → stereo)
    → swresample (resample if needed)
    → convertFloatPcmToS16
    → AudioTrack (STEREO PCM)
    → AudioFlinger Mixer (STEREO)
    → Audio HAL (MI_PCM path)
    → Speakers (2.0)
```

### Path B: Compressed Passthrough / Offload (Hardware Path)
```
Compressed Audio (AC3/E-AC3/DTS/DTS-HD/AAC)
    → MediaCodec (OMX.MS.* hardware decoders OR passthrough)
    → Offload output (audio_policy_configuration.xml)
    → Audio HAL (MI_AUDIO path via mi_common_raw_write)
    → Hardware decoder in TV SoC
    → HDMI/SPDIF/ARC (compressed bitstream)
    → External AVR/Soundbar (decodes externally)
```

---

## FACT: Property Experiment Confirmation

### Experiment: Changed persist.vendor.pie.5_1.center from 0.90 → 0.0
- Property change accepted by system
- Decoder reads property at runtime (trace string includes `c=%.3f`)
- **Predicted**: Audible center channel level in downmix would change
- **Status**: Requires playback test to confirm audible difference

---

## UNVERIFIED HYPOTHESIS

1. **Decoder always downs to stereo** - No code path found for multichannel PCM output from this decoder
2. **7.1 downmix uses side+rear coefficients** - `r=%.3f` in trace for 7.1 rear channels
3. **Atmos/JOC metadata discarded** - Decoder outputs PCM only, trace shows `isAtmos=%d` but no metadata pass-through
4. **TrueHD uses same downmix path** - Same decoder library, same downmixToStereoFloat function
5. **HAL's utils_convert_channel is unused for this path** - Only used for other input paths (USB, BT, HDMI PCM in)

---

## MISSING BASELINE
- Original/stock `audio.primary.mt5862.so` - not available for diff
- Stock `media_codecs.xml` without OMX.google.* software decoders - not available
- Stock `libstagefright_soft_ac3dec.so` (separate per-format decoders) - not available

---

## Architecture Implications

### For Clean Rewrite (Candidate A): Decoder Produces Multichannel → Custom Downmix → Stereo
**Integration Point**: Replace `libstagefright_soft_ac3dec.so` with new unified decoder
- New decoder outputs multichannel PCM (configurable)
- New custom downmix engine (separate library) does 5.1/7.1 → 2.1
- Output stereo PCM to existing Android path
- **Pros**: Clean separation, audiophile-quality downmix, configurable
- **Cons**: Must replicate decoder's current behavior exactly

### For Clean Rewrite (Candidate B): Decoder Produces Multichannel → Android Multichannel Path → Downstream Downmix
**Integration Point**: Modify `audio_policy_configuration.xml` to add multichannel PCM to primary output
- New decoder outputs multichannel PCM
- AudioFlinger mixer handles multichannel → stereo (via libdownmix.so or custom effect)
- **Pros**: Uses Android framework multichannel support
- **Cons**: Requires HAL to accept multichannel PCM (may need HAL mod), more invasive

### Current System = Implicit Candidate A (but downmix inside decoder)
The current system is essentially Candidate A but with the downmix baked into the decoder library.

---

## Recommendation

**Adopt Candidate A architecture** but with clean separation:
1. **Decoder backend** → outputs multichannel PCM (no downmix)
2. **Downmix engine** → separate, configurable, audiophile-quality
3. **PCM normalization** → format conversion, resampling
4. **Android integration** → OMX component, stereo output to existing path

This matches the current working behavior but with:
- Clean modular architecture
- Configurable downmix (not hardcoded in decoder)
- Proper channel mapping verification
- Reversible installation

The persistent properties (`persist.vendor.pie.*`) should be migrated to a formal config file with the decoder as a compatibility layer.
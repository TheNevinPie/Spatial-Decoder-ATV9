# Final Architecture Candidates - Evidence-Based

## Executive Summary

**CONCLUSIVE FACT**: The downmix from 5.1/7.1 → stereo/2.1 happens **inside the decoder library** (`libstagefright_soft_ac3dec.so`), not in AudioFlinger or Audio HAL.

**Evidence**:
1. Binary contains `downmixToStereoFloat` function
2. Trace string logs downmix coefficients (c, s, r, lfe, matrix, trim)
3. Decoder reads `persist.vendor.pie.*` properties directly
4. AudioFlinger primary output: STEREO ONLY, 0 Effect Chains
5. HAL `downmix_mode: 0`, no downmix effect chain active

---

## Candidate A: Decoder → Custom Downmix → Stereo PCM → Existing Android Path

### Architecture
```
Compressed Audio (AC3/E-AC3/DTS/TrueHD)
    │
    ▼
MTK Extractor (MIME: audio/ac3, audio/eac3, audio/vnd.dts, etc.)
    │
    ▼
MediaCodec → OMX.spatialdecoder.ac3 / eac3 / dts / truehd
    │
    ▼
SoftOMXPlugin → New Unified Decoder (libspatialdecoder.so)
    │
    ├── FFmpeg Backend (libavcodec) → Multichannel PCM (float planar)
    │
    ▼
PCM Normalization Layer
    │   ├── Resampling (libswresample)
    │   ├── Channel layout normalization
    │   └── Format conversion (float → S16)
    │
    ▼
Custom Downmix Engine (libspatialdownmix.so)
    │   ├── Input: 5.1 / 7.1 / stereo layouts
    │   ├── Output: 2.1 (L, R, LFE) 
    │   ├── Configurable matrix coefficients
    │   ├── Bass management (LFE + redirected bass)
    │   ├── Peak limiting / soft clipping
    │   └── Config via file + system properties
    │
    ▼
Stereo/2.1 PCM (S16 interleaved)
    │
    ▼
AudioTrack (STEREO) → AudioFlinger (STEREO) → Audio HAL (MI_PCM) → Speakers
```

### Integration Points
| Component | Change Required |
|-----------|-----------------|
| `media_codecs.xml` | Replace OMX.google.* with OMX.spatialdecoder.* |
| `libspatialdecoder.so` | New unified decoder (vendor/lib) |
| `libspatialdownmix.so` | New downmix engine (vendor/lib) |
| `audio_policy_configuration.xml` | **NO CHANGE** (primary output stays stereo) |
| `audio_effects.xml` | **NO CHANGE** (downmix not in effect chain) |
| Audio HAL | **NO CHANGE** (receives stereo PCM as today) |

### Pros
- ✅ Minimal system modification (only media_codecs.xml + 2 new libraries)
- ✅ Fully reversible (remove libraries, restore media_codecs.xml)
- ✅ No HAL modification risk
- ✅ No AudioFlinger policy change risk
- ✅ Clean separation: decoder | PCM | downmix
- ✅ Audiophile-quality downmix with full configurability
- ✅ Compatible with all existing apps (standard MediaCodec API)
- ✅ Preserves compressed passthrough/offload path unchanged

### Cons
- ⚠️ Must replicate decoder's current behavior exactly
- ⚠️ Downmix coefficients must match or improve current sound

### CPU Cost Estimate
- FFmpeg decode: ~5-15% CPU (armv7, 5.1 @ 48kHz)
- Downmix: ~1-2% CPU (6→2 channels, FIR/IIR)
- Resampling: ~1-3% CPU (if needed)
- **Total: ~7-20% CPU** (acceptable for TV SoC)

---

## Candidate B: Decoder → Multichannel PCM → Android Multichannel Path → Downstream Downmix

### Architecture
```
Compressed Audio
    │
    ▼
MediaCodec → OMX.spatialdecoder.*
    │
    ▼
New Unified Decoder → Multichannel PCM (5.1/7.1 float)
    │
    ▼
AudioTrack (MULTICHANNEL: 5.1 or 7.1)
    │
    ▼
AudioFlinger Primary Output (MULTICHANNEL capable)
    │
    ▼
Audio HAL (MI_PCM with 6/8 channels)
    │
    ▼
Downmix in HAL or Post-HAL DSP → 2.1 Speaker Output
```

### Integration Points
| Component | Change Required |
|-----------|-----------------|
| `media_codecs.xml` | Add OMX.spatialdecoder.* |
| `audio_policy_configuration.xml` | **ADD** multichannel PCM profiles to primary output |
| `audio.primary.mt5862.so` | **MODIFY** to accept 6/8 channel PCM, configure MI_PCM |
| Audio HAL | **MODIFY** downmix logic or add DSP downmix |
| `libspatialdecoder.so` | New decoder (outputs multichannel) |

### Pros
- ✅ Cleaner Android framework integration (uses standard multichannel path)
- ✅ Downmix in HAL/DSP could be more efficient
- ✅ Future-proof for multichannel speaker upgrades

### Cons
- ❌ **Requires HAL modification** (high risk, no stock baseline)
- ❌ **Requires audio_policy_configuration.xml change** (affects all audio)
- ❌ HAL must support multichannel PCM input (MI_PCM_Open with 6/8 ch)
- ❌ Downmix in HAL is opaque (MediaTek proprietary MI_* APIs)
- ❌ Risk of breaking compressed passthrough
- ❌ Risk of breaking HDMI/SPDIF/ARC output
- ❌ No stock HAL to diff against
- ❌ Much harder to reverse

### CPU Cost
- Similar decode cost
- Downmix in HAL/DSP: potentially lower CPU but opaque

---

## Evidence Comparison

| Factor | Candidate A | Candidate B |
|--------|-------------|-------------|
| **Current working system behavior** | MATCHES (decoder does downmix) | DIFFERENT (would require HAL change) |
| **AudioFlinger primary output** | STEREO (no change) | Must become MULTICHANNEL |
| **HAL downmix_mode** | 0 (unused) | Must be implemented/enabled |
| **libdownmix.so effect** | Not used (confirmed) | Would need activation |
| **Risk of bootloop/audio loss** | LOW | HIGH |
| **Reversibility** | TRIVIAL | DIFFICULT |
| **HAL modification needed** | NO | YES |
| **Policy modification needed** | NO | YES |
| **Compressed passthrough preserved** | YES (unchanged) | RISK |
| **Stock baseline available for diff** | N/A | NO (critical gap) |

---

## Recommendation: CANDIDATE A

**Overwhelming evidence favors Candidate A:**

1. **Current system proves it works**: The decoder-internal downmix path is production-working
2. **Zero HAL risk**: HAL receives stereo PCM exactly as today
3. **Zero policy risk**: AudioFlinger unchanged
4. **Trivial reversibility**: Remove 2 libraries, restore 1 XML file
5. **Clean architecture**: Decoder | PCM | Downmix as separate modules
6. **No missing baselines**: We don't need stock HAL or policy

### Implementation Priority for Candidate A

#### Phase 1: Decoder Backend (libspatialdecoder.so)
- FFmpeg wrapper for AC3/E-AC3/DTS/TrueHD
- Outputs multichannel float planar PCM
- OMX component plugin (SoftOMXPlugin replacement)

#### Phase 2: PCM Normalization
- Resampling (libswresample)
- Channel layout normalization
- Float → S16 conversion

#### Phase 3: Downmix Engine (libspatialdownmix.so)
- Configurable matrix coefficients
- Bass management
- Peak limiting
- Property/file config system

#### Phase 4: Android Integration
- OMX component registration
- media_codecs.xml update
- Install/verify scripts

---

## Migration of Persistent Properties

Current properties → New config system:
```
persist.vendor.pie.5_1.center      → downmix.coeff.5_1.center
persist.vendor.pie.5_1.surround    → downmix.coeff.5_1.surround
persist.vendor.pie.5_1.lfe         → downmix.coeff.5_1.lfe
persist.vendor.pie.5_1.width       → downmix.coeff.5_1.width
persist.vendor.pie.7_1.center      → downmix.coeff.7_1.center
persist.vendor.pie.7_1.side        → downmix.coeff.7_1.side
persist.vendor.pie.7_1.rear        → downmix.coeff.7_1.rear
persist.vendor.pie.7_1.lfe         → downmix.coeff.7_1.lfe
persist.vendor.pie.7_1.width       → downmix.coeff.7_1.width
persist.vendor.pie.atmos.matrix_encoding → downmix.matrix.atmos
persist.vendor.pie.surround.matrix_encoding → downmix.matrix.surround
```

Compatibility: Decoder reads properties as fallback if config file absent.

---

## Risk Register (Candidate A)

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Decoder doesn't match old output | Medium | High | Channel mapping test tones, A/B comparison |
| Downmix sounds different | Medium | Medium | Preserve exact coefficients, blind listening test |
| FFmpeg version incompatibility | Low | High | Build against same NDK/API level, same configure flags |
| OMX component not registered | Low | High | Verify with `dumpsys media.codec` post-install |
| SELinux denial | Low | Medium | Include sepolicy module |
| Breaks hardware passthrough | Very Low | High | Offload path unchanged in policy |
| CPU overload | Low | Medium | Benchmark, optimize buffer sizes |

---

## Files to Modify (Minimal Set)

### Add:
- `/vendor/lib/libspatialdecoder.so` - Unified decoder + PCM normalization
- `/vendor/lib/libspatialdownmix.so` - Downmix engine
- `/vendor/etc/spatialdecoder_config.json` - Downmix configuration

### Modify:
- `/vendor/etc/media_codecs.xml` - Replace OMX.google.* with OMX.spatialdecoder.*

### Backup (before install):
- `/vendor/etc/media_codecs.xml`
- `/vendor/etc/audio_policy_configuration.xml` (unchanged but backup)
- `/vendor/etc/audio_effects.xml` (unchanged but backup)

---

## Verification Checklist (Post-Install)

1. ✅ Codec registered: `dumpsys media.codec | grep spatialdecoder`
2. ✅ AC3 5.1 plays → stereo output
3. ✅ E-AC3 5.1 plays → stereo output
4. ✅ E-AC3 7.1 plays → stereo output
5. ✅ DTS 5.1 plays → stereo output
6. ✅ TrueHD plays → stereo output
7. ✅ Channel mapping test: unique tone per channel → verify L/R/LFE at speaker
8. ✅ Property override: change center gain → audible change
9. ✅ CPU usage < 25% during decode
10. ✅ No audio underruns in logcat
11. ✅ Compressed passthrough still works (HDMI/SPDIF)
12. ✅ Uninstall restores original behavior
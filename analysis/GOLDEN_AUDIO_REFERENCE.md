# Golden Audio Reference - Old Decoder Behavior

## Source of Truth
Binary analysis of `libstagefright_soft_ac3dec.so` (SHA256: 815a8229903843aba52515af03f0a50caa67a2938f16e826622375dd9e1ca6b8)

---

## Decoder Output Format (CONFIRMED)

From AudioFlinger dump:
```
Output thread AudioOut_D:
  Channel count: 2
  Channel mask: 0x00000003 (front-left, front-right)
  Processing format: 0x5 (AUDIO_FORMAT_PCM_FLOAT)
  HAL format: 0x1 (AUDIO_FORMAT_PCM_16_BIT)
```

**Decoder outputs STEREO PCM (S16 interleaved) directly** - no multichannel PCM ever reaches AudioFlinger.

---

## Downmix Parameters (From Binary Trace String + Properties)

### Trace String Format (from binary):
```
TRACE %s downmix bucket=%s layout=%s isAtmos=%d profileId=%d matrix=%s 
    c=%.3f s=%.3f r=%.3f lfe=%.3f 
    trimMode=postmix_peak trimTarget=%.3f releaseMs=%.0f lfeLp=%d 
    loudness=strict_reference props=%s,%s,%s,%s,%s,%s,%s,%s,%s
```

### 5.1 Downmix (persist.vendor.pie.5_1.*)
| Parameter | Property | Value | Description |
|-----------|----------|-------|-------------|
| Center gain (c) | persist.vendor.pie.5_1.center | **0.90** | Center channel contribution |
| Surround gain (s) | persist.vendor.pie.5_1.surround | **0.55** | Surround (LS/RS) contribution |
| LFE gain (lfe) | persist.vendor.pie.5_1.lfe | **0.25** | LFE channel contribution |
| Width | persist.vendor.pie.5_1.width | **0.95** | Stereo width factor |
| Matrix encoding | persist.vendor.pie.surround.matrix_encoding | **dplii** | Dolby Pro Logic II |
| Trim mode | (hardcoded) | postmix_peak | Peak limiting after mix |
| Loudness | (hardcoded) | strict_reference | Reference loudness compliance |

### 7.1 Downmix (persist.vendor.pie.7_1.*)
| Parameter | Property | Value | Description |
|-----------|----------|-------|-------------|
| Center gain (c) | persist.vendor.pie.7_1.center | **0.760** | Center channel contribution |
| Side gain (s) | persist.vendor.pie.7_1.side | **0.780** | Side surround (LS/RS) contribution |
| Rear gain (r) | persist.vendor.pie.7_1.rear | **0.620** | Rear surround (LRS/RRS) contribution |
| LFE gain (lfe) | persist.vendor.pie.7_1.lfe | **0.030** | LFE channel contribution |
| Width | persist.vendor.pie.7_1.width | **1.03** | Stereo width factor |
| Matrix encoding | persist.vendor.pie.atmos.matrix_encoding | **dplii** | Dolby Pro Logic II |
| Trim mode | (hardcoded) | postmix_peak | Peak limiting after mix |
| Loudness | (hardcoded) | strict_reference | Reference loudness compliance |

---

## Decoder Internal Behavior (From Symbols)

### Functions
```
downmixToStereoFloat - Main downmix function
convertFloatPcmToS16 - Float to S16 conversion with stats
ensureResampler - Initializes swresample
flushResamplerToPcm - Drains resampler
processPendingTrueHdBytes - TrueHD sync handling
```

### Resampler Configuration (swresample)
From trace: `TRACE %s source channels=%d rate=%d layout=%s fmt=%d`
- Input: Decoded multichannel float planar (from FFmpeg)
- Output: Stereo float planar (for downmix) → S16 interleaved
- Sample rate: Stream native (typically 48000 Hz)
- Channel layout conversion: 5.1/7.1 → stereo (via downmixToStereoFloat, NOT swresample)

### Channel Mapping (FFmpeg Standard)
| 5.1 (AV_CH_LAYOUT_5POINT1) | 7.1 (AV_CH_LAYOUT_7POINT1) |
|----------------------------|----------------------------|
| FL (Front Left) | FL (Front Left) |
| FR (Front Right) | FR (Front Right) |
| FC (Front Center) | FC (Front Center) |
| LFE (Low Frequency) | LFE (Low Frequency) |
| SL (Side Left) | SL (Side Left) |
| SR (Side Right) | SR (Side Right) |
| | BL (Back Left) |
| | BR (Back Right) |

---

## Format-Specific Behavior

### AC-3 (audio/ac3)
- Component: OMX.google.ac3.decoder → "ac3dec"
- FFmpeg codec: AV_CODEC_ID_AC3
- Parser: AV_CODEC_ID_AC3
- Max channels: 6 (5.1)
- Sample rates: 32000, 44100, 48000

### E-AC-3 (audio/eac3, audio/eac3-joc)
- Component: OMX.google.eac3.decoder → "eac3dec"
- FFmpeg codec: AV_CODEC_ID_EAC3
- Parser: AV_CODEC_ID_AC3 (same as AC-3)
- Max channels: 8 (7.1) or 16 (JOC/Atmos)
- Sample rates: 32000, 44100, 48000
- **Atmos/JOC metadata: DISCARDED** - only PCM output

### DTS (audio/vnd.dts)
- Component: OMX.google.dts.decoder → "dtsdec"
- FFmpeg codec: AV_CODEC_ID_DCA
- Parser: AV_CODEC_ID_DCA
- Max channels: 8
- Sample rates: 8000-192000

### DTS-HD / DTS-ES / DTS-LBR
- Same component: "dtsdec" (FFmpeg DCA decoder handles all)
- **Only core (lossy) layer decoded** - no XLL lossless

### TrueHD (audio/true-hd)
- Component: OMX.google.truehd.decoder → "truehddec"
- FFmpeg codec: AV_CODEC_ID_TRUEHD
- Parser: AV_CODEC_ID_MLP
- Special handling: Major sync frame detection (0xF8726FBA)
- Max channels: 8
- Sample rates: 44100, 48000, 96000, 192000

---

## Downmix Matrix (Conceptual)

### 5.1 → Stereo
```
L_out = FL + c*FC + s*SL + s*SR + lfe*LFE
R_out = FR + c*FC + s*SR + s*SL + lfe*LFE
```
Where c=0.90, s=0.55, lfe=0.25

### 7.1 → Stereo
```
L_out = FL + c*FC + s*SL + r*BL + lfe*LFE
R_out = FR + c*FC + s*SR + r*BR + lfe*LFE
```
Where c=0.760, s=0.780, r=0.620, lfe=0.030

### Post-Mix Processing
1. **Trim/Limiter**: postmix_peak mode with trimTarget
2. **LFE Low-pass**: lfeLp enabled (likely 120-200Hz crossover)
3. **Loudness**: strict_reference compliance

---

## Reference Test Files (Generated)

| File | Format | Channels | Channel ID Tones |
|------|--------|----------|------------------|
| test_ac3_51_chid.ac3 | AC-3 | 5.1 | FL=440, FR=550, FC=660, LFE=80, SL=770, SR=880 |
| test_eac3_51_chid.ec3 | E-AC-3 | 5.1 | Same as above |
| test_eac3_71_chid.ec3 | E-AC-3 | 7.1 | FL=440, FR=550, FC=660, LFE=80, SL=770, SR=880, BL=990, BR=1100 |
| test_dts_51_chid.dts | DTS | 5.1 | Same as AC-3 5.1 |
| test_truehd_51_chid.thd | TrueHD | 5.1 | Same as AC-3 5.1 |

### Expected Audible Output (2.1 Speaker)
| Channel | Expected in Left | Expected in Right | Expected in LFE |
|---------|-----------------|------------------|-----------------|
| FL (440Hz) | **FULL** | - | - |
| FR (550Hz) | - | **FULL** | - |
| FC (660Hz) | 0.90/0.76 | 0.90/0.76 | - |
| LFE (80Hz) | 0.25/0.03 | 0.25/0.03 | **FULL** (via bass mgmt) |
| SL (770Hz) | 0.55/0.78 | 0.55/0.78 | - |
| SR (880Hz) | 0.55/0.78 | 0.55/0.78 | - |
| BL (990Hz) | - | - | 0.62 (7.1 only) |
| BR (1100Hz) | - | - | 0.62 (7.1 only) |

---

## Verification Checklist for New Implementation

For each format/layout, the new decoder must produce bit-equivalent (or near-bit-equivalent) output:

- [ ] AC3 5.1 → Stereo PCM, 48kHz, S16
- [ ] E-AC3 5.1 → Stereo PCM, 48kHz, S16  
- [ ] E-AC3 7.1 → Stereo PCM, 48kHz, S16
- [ ] DTS 5.1 → Stereo PCM, 48kHz, S16
- [ ] TrueHD 5.1 → Stereo PCM, 48kHz, S16

### Coefficient Verification
- [ ] 5.1 center = 0.90 ± 0.001
- [ ] 5.1 surround = 0.55 ± 0.001
- [ ] 5.1 lfe = 0.25 ± 0.001
- [ ] 7.1 center = 0.760 ± 0.001
- [ ] 7.1 side = 0.780 ± 0.001
- [ ] 7.1 rear = 0.620 ± 0.001
- [ ] 7.1 lfe = 0.030 ± 0.001

### Property Override Test
- [ ] Setting center=0.0 mutes center in downmix
- [ ] Setting center=2.0 boosts center 2x
- [ ] Setting surround=0.0 mutes surrounds
- [ ] Setting lfe=0.0 mutes LFE
- [ ] Property changes take effect without restart (decoder reads on init)

### Quality Checks
- [ ] No audible clipping at full-scale input
- [ ] No channel inversion
- [ ] Correct channel mapping (channel ID test)
- [ ] CPU usage < 25% on MT5862
- [ ] No audio underruns in 30min playback

---

## MISSING REFERENCE
- Exact trimTarget value (not in properties, likely hardcoded)
- Exact releaseMs value
- Exact lfeLp crossover frequency
- TrueHD operational status (special sync handling may be fragile)

These require runtime capture or reverse engineering of the downmixToStereoFloat function.
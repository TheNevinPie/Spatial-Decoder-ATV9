# Codec Selection Mechanism - Findings

## How Software Decoder Wins Over Hardware

### 1. MediaCodecList Construction
- Built from `media_codecs.xml` + `media_codecs_performance.xml`
- Parsed by `MediaCodecsXmlParser` (framework)
- Both hardware (OMX.MS.*) and software (OMX.google.*) decoders declared

### 2. Codec Ranking/Selection
Framework string: `Codec resolved it to (R:%d(%s), P:%d(%s), M:%d(%s), T:%d(%s)) err=%d(%s)`
- R, P, M, T = ranking dimensions (likely: Rank, Performance, Match, Type)
- Software decoders can be preferred via:
  - Higher performance scores in `media_codecs_performance.xml`
  - Explicit app request for software decoder
  - Hardware decoder not supporting the format/profile

### 3. Current media_codecs.xml Declares Both
**Hardware (MTK):**
```
OMX.MS.AC3.Decoder (if exists)
OMX.MS.EAC3.Decoder (if exists)
OMX.MS.DTS.Decoder (if exists)
```

**Software (Google):**
```
OMX.google.ac3.decoder
OMX.google.eac3.decoder
OMX.google.dts.decoder
OMX.google.dtshd.decoder
OMX.google.dtse.decoder
OMX.google.dtslbr.dec
OMX.google.truehd.decoder
```

### 4. Selection Factors
From Android framework (MediaCodecList):
- Codec capabilities match (format, channels, sample rate, bitrate)
- Performance points from `media_codecs_performance.xml`
- Encoder/decoder type preference
- Secure vs non-secure
- App can request software-only via `MediaCodecList.REGULAR` vs `SECURE`

### 5. No Explicit "Disable Hardware" Needed
The software decoder is selected because:
- It's declared in media_codecs.xml with appropriate capabilities
- Performance XML may favor it for certain profiles
- Hardware decoder may not support all profiles (e.g., TrueHD, DTS-HD XLL)

---

## Key Insight: media_codecs.xml IS the Integration Point

The cleanest integration for a new decoder is:
1. Add new OMX component names to `media_codecs.xml`
2. Implement SoftOMXPlugin that registers them
3. Framework handles selection automatically

No framework binary patching required.
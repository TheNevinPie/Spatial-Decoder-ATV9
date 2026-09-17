# OMX Software Component Registration Mechanism - Android 9 (MT5862)

## How It Works (CONCLUSIVE)

### Registration Flow
```
Application requests decoder for MIME type (e.g., audio/ac3)
    │
    ▼
MediaCodecList (from media_codecs.xml)
    │
    ▼
OMXMaster → SoftOMXPlugin
    │
    ▼
SoftOMXPlugin.makeComponentInstance("OMX.google.ac3.decoder", ...)
    │
    ▼
Internal name mapping (HARDCODED in SoftOMXPlugin):
    "OMX.google.ac3.decoder"       → "ac3dec"
    "OMX.google.eac3.decoder"      → "eac3dec"
    "OMX.google.dts.decoder"       → "dtsdec"
    "OMX.google.dtshd.decoder"     → "dtsdec"
    "OMX.google.dtse.decoder"      → "dtsdec"
    "OMX.google.dtslbr.dec"        → "dtsdec"
    "OMX.google.truehd.decoder"    → "truehddec"
    │
    ▼
createSoftOMXComponent(internal_name, ...)
    │
    ▼
Constructs library name: "libstagefright_soft_" + internal_name + ".so"
    │
    ▼
dlopen("libstagefright_soft_ac3dec.so") etc.
    │
    ▼
dlsym("createSoftOMXComponent") → function pointer
    │
    ▼
Call createSoftOMXComponent(internal_name, callbacks, appData, component)
    │
    ▼
Creates SoftAc3Omx (for ALL formats)
```

### Critical Finding: All 4 Decoder Libraries Are IDENTICAL
| Library | SHA256 | Size |
|---------|--------|------|
| libstagefright_soft_ac3dec.so | 815a8229... | 58,276 |
| libstagefright_soft_eac3dec.so | 815a8229... | 58,276 |
| libstagefright_soft_dtsdec.so | 815a8229... | 58,276 |
| libstagefright_soft_truehddec.so | 815a8229... | 58,276 |

**They are the SAME FILE** (hardlinks or copies). The single library handles ALL formats via the internal name passed to `createSoftOMXComponent`.

### The createSoftOMXComponent Function
Located in `libstagefright_soft_ac3dec.so` (and the other 3 identical copies):
```cpp
// Exported symbol: createSoftOMXComponent
// Signature: createSoftOMXComponent(const char* name, OMX_CALLBACKTYPE*, void*, OMX_COMPONENTTYPE**)
// Behavior: Creates SoftAc3Omx which internally selects codec based on 'name'
```

### SoftAc3Omx Codec Selection (Internal)
From symbol analysis in the decoder library:
- "ac3dec" → AV_CODEC_ID_AC3
- "eac3dec" → AV_CODEC_ID_EAC3
- "dtsdec" → AV_CODEC_ID_DCA (handles DTS, DTS-HD, DTS-ES, DTS-LBR)
- "truehddec" → AV_CODEC_ID_TRUEHD

---

## Compatibility Strategy: KEEP OMX.google.* Names

### Minimal Modification Path (NO libstagefright_omx.so change required)

**Option A: Replace 4 Libraries with 1 (RECOMMENDED)**
```
1. Build libspatialdecoder.so with createSoftOMXComponent export
2. Replace on device:
   /system/lib/libstagefright_soft_ac3dec.so       → libspatialdecoder.so
   /system/lib/libstagefright_soft_eac3dec.so      → libspatialdecoder.so
   /system/lib/libstagefright_soft_dtsdec.so       → libspatialdecoder.so
   /system/lib/libstagefright_soft_truehddec.so    → libspatialdecoder.so
   /system/lib/vndk-28/libstagefright_soft_ac3dec.so   → libspatialdecoder.so
   (same for vndk-28 copies)
3. Keep media_codecs.xml with OMX.google.* entries
4. DONE - zero framework modification
```

**Why this works:**
- SoftOMXPlugin still maps OMX.google.ac3.decoder → "ac3dec"
- createSoftOMXComponent("ac3dec") tries to load "libstagefright_soft_ac3dec.so"
- That file IS NOW libspatialdecoder.so (replaced)
- libspatialdecoder.so exports createSoftOMXComponent
- Function receives "ac3dec" and creates decoder for AC3
- Same for eac3dec, dtsdec, truehddec

**Option B: Modify libstagefright_omx.so (More Invasive)**
- Change library name construction from "libstagefright_soft_" + name + ".so" to "libspatialdecoder.so" for audio decoder names
- Single binary patch
- But requires modifying a core framework library

**Option C: New Plugin + New Component Names (Most Invasive)**
- Add OMX.spatialdecoder.* to media_codecs.xml
- Create new OMX plugin library
- Modify OMXMaster to load it
- Requires framework changes

---

## Verification: Current Behavior is "Create Once, Use for All"

The `createSoftOMXComponent` function in the decoder library:
1. Receives internal name (e.g., "ac3dec")
2. Creates `new SoftAc3Omx(name, ...)`
3. SoftAc3Omx::initDecoder() uses `name` to select FFmpeg codec:
   - strcmp(name, "ac3dec") == 0 → AV_CODEC_ID_AC3
   - strcmp(name, "eac3dec") == 0 → AV_CODEC_ID_EAC3
   - strcmp(name, "dtsdec") == 0 → AV_CODEC_ID_DCA
   - strcmp(name, "truehddec") == 0 → AV_CODEC_ID_TRUEHD

**Our new libspatialdecoder.so must replicate this exact behavior.**

---

## Implementation Plan for OMX Registration

### Phase 1: Build libspatialdecoder.so
```cpp
// Exported C function (matching existing signature)
extern "C" OMX_ERRORTYPE createSoftOMXComponent(
    const char* name,
    const OMX_CALLBACKTYPE* callbacks,
    void* appData,
    OMX_COMPONENTTYPE** component
) {
    // Create our decoder component that handles all formats
    // based on 'name' parameter
    return new SpatialDecoderOMX(name, callbacks, appData, component);
}
```

### Phase 2: Install
```bash
# Backup originals
cp /system/lib/libstagefright_soft_ac3dec.so /data/adb/backup/
cp /system/lib/libstagefright_soft_eac3dec.so /data/adb/backup/
cp /system/lib/libstagefright_soft_dtsdec.so /data/adb/backup/
cp /system/lib/libstagefright_soft_truehddec.so /data/adb/backup()

# Replace with new unified library
cp libspatialdecoder.so /system/lib/libstagefright_soft_ac3dec.so
cp libspatialdecoder.so /system/lib/libstagefright_soft_eac3dec.so
cp libspatialdecoder.so /system/lib/libstagefright_soft_dtsdec.so
cp libspatialdecoder.so /system/lib/libstagefright_soft_truehddec.so

# Same for VNDK copies
cp libspatialdecoder.so /system/lib/vndk-28/libstagefright_soft_ac3dec.so
# ... etc

# Set permissions and selinux labels
chmod 644 /system/lib/libstagefright_soft_*.so
chown root:root /system/lib/libstagefright_soft_*.so
restorecon /system/lib/libstagefright_soft_*.so

# Restart media server
setprop ctl.restart media
```

### Phase 3: Verify
```bash
# Check codec registration
dumpsys media.codec | grep -E "ac3|eac3|dts|truehd"

# Test playback
# Play AC3 5.1 → verify OMX.google.ac3.decoder selected
# Play E-AC3 5.1 → verify OMX.google.eac3.decoder selected
# Play DTS 5.1 → verify OMX.google.dts.decoder selected
# Play TrueHD → verify OMX.google.truehd.decoder selected
```

---

## Answers to Registration Questions

| Question | Answer |
|----------|--------|
| 1. Is SoftOMXPlugin hard-coded to known component names? | **YES** - mapping table is hardcoded in SoftOMXPlugin |
| 2. Can new OMX.spatialdecoder.* be registered purely via media_codecs.xml? | **NO** - SoftOMXPlugin doesn't know these names; would need new plugin |
| 3. Does plugin dynamically load library based on component name? | **YES** - constructs "libstagefright_soft_" + internal_name + ".so" |
| 4. Does libstagefright_omx.so contain hard-coded factory table? | **YES** - in SoftOMXPlugin::makeComponentInstance() |
| 5. What was changed in old libstagefright_omx.so? | **Mapping added** for OMX.google.ac3/eac3/dts/truehd → ac3dec/eac3dec/dtsdec/truehddec |
| 6. Can same registration be achieved with smaller modification? | **YES** - replace 4 decoder .so files with 1 unified library |
| 7. Can we retain OMX.google.* names while replacing implementation? | **YES** - Option A above (RECOMMENDED) |
| 8. Can new registration mechanism coexist? | **YES** but requires new plugin + framework changes |

---

## Golden Audio Reference Extraction (Next Step)

Before implementing libspatialdecoder.so, extract reference behavior from current decoder:

### For each format:
| Format | Test File | Expected Output |
|--------|-----------|-----------------|
| AC3 5.1 | test_ac3_51_chid.ac3 | Stereo PCM, 48kHz, S16 |
| E-AC3 5.1 | test_eac3_51_chid.ec3 | Stereo PCM, 48kHz, S16 |
| E-AC3 7.1 | test_eac3_71_chid.ec3 | Stereo PCM, 48kHz, S16 |
| DTS 5.1 | (need test file) | Stereo PCM, 48kHz, S16 |
| TrueHD | (need test file) | Stereo PCM, 48kHz, S16 |

### Capture for each:
1. Decoded channel layout (before downmix)
2. Output sample format/rate/channels
3. Exact downmix coefficients from properties
4. Headroom/limiter behavior
5. Reference WAV output file

### Method:
- Hook into decoder output (logcat trace strings already show coefficients)
- Or use `dumpsys media.audio_flinger` to capture AudioTrack output
- Or write a test app that captures AudioTrack write() calls

The trace string already gives us the coefficients:
```
TRACE %s downmix bucket=%s layout=%s isAtmos=%d profileId=%d matrix=%s 
    c=%.3f s=%.3f r=%.3f lfe=%.3f 
    trimMode=postmix_peak trimTarget=%.3f releaseMs=%.0f lfeLp=%d 
    loudness=strict_reference props=%s...
```

Current property values:
- 5.1: center=0.90, surround=0.55, lfe=0.25, width=0.95
- 7.1: center=0.760, side=0.780, rear=0.620, lfe=0.030, width=1.03
- matrix_encoding: dplii
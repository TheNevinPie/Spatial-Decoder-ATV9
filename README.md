# Spatial-Decoder-ATV9

System-wide software Dolby/DTS decoder and multichannel downmix for Android TV 9.

## Features

- Software audio decoding
- 5.1 and 7.1 to stereo downmix
- Per-channel volume control
- Runtime configuration
- FFmpeg-based architecture
- Android OMX integration

## Status

🚧 In development.

Current focus: decoder integration and Android deployment.

## Note

Built for research and development on Android TV 9 / MediaTek platforms.

## Documentation

| Document | Location |
|----------|----------|
| Downmix mathematical contract (frozen) | `docs/MATHEMATICAL_CONTRACT.md` |
| Phase 2 final report | `docs/FINAL_REPORT.md` |
| Phase 2 test report | `docs/TEST_REPORT.md` |
| Reverse-engineering notes (historical) | `analysis/` (see `analysis/README.md`) |

## Runtime channel-gain properties

Every downmix channel gain is tunable at runtime through Android system
properties. Values are **linear gain multipliers**: `1.0` = unity,
`0.5` = -6.02 dB, `0.70710678` ≈ -3.01 dB, `0.0` = muted, `2.0` = +6.02 dB.
Values must be finite and `>= 0`; NaN/Inf/negative/unparseable values are
rejected and the previous/default gain is kept. Gains above 1.0 are
allowed; float clipping behavior is unchanged (saturating S16 cast, no
limiter/normalization).

Stereo output matrix (L = Σ in[c]·m[c][0], R = Σ in[c]·m[c][1]):

| Input channel | Left coeff | Right coeff |
|---|---|---|
| FL / FR | `gain_FL` / 0 | 0 / `gain_FR` |
| C | `gain_C × 0.70710678` | `gain_C × 0.70710678` |
| SL / SR (5.1) | `gain_SL × 0.70710678` | `gain_SL × 0.70710678` (SR likewise) |
| Side L/R, Rear L/R (7.1) | `gain × 0.70710678` each | same |
| LFE | `gain_LFE × 1.0` | `gain_LFE × 1.0` |

### 5.1 properties (`persist.vendor.spatialdm.5_1.*`)

| Property | Channel | Default | Meaning |
|---|---|---|---|
| `front_left` | FL | 1.0 | front-left linear gain |
| `front_right` | FR | 1.0 | front-right linear gain |
| `center` | C | 0.90 | center linear gain (×0.7071 into L/R) |
| `lfe` | LFE | 0.25 | LFE linear gain (×1.0 into L/R) |
| `surround_left` | SL | = surround | left-surround linear gain (×0.7071) |
| `surround_right` | SR | = surround | right-surround linear gain (×0.7071) |
| `left` / `right` / `surround` (compat) | FL / FR / SL+SR | 1.0 / 1.0 / 0.55 | legacy shared gains; a set fine-grained key wins for its own channel |

### 7.1 properties (`persist.vendor.spatialdm.7_1.*`)

| Property | Channel | Default | Meaning |
|---|---|---|---|
| `front_left` | FL | 1.0 | front-left linear gain |
| `front_right` | FR | 1.0 | front-right linear gain |
| `center` | C | 0.760 | center linear gain (×0.7071 into L/R) |
| `lfe` | LFE | 0.030 | LFE linear gain (×1.0 into L/R) |
| `side_left` | SL | = side | left-side linear gain (×0.7071) |
| `side_right` | SR | = side | right-side linear gain (×0.7071) |
| `rear_left` | BL | = rear | left-rear linear gain (×0.7071) |
| `rear_right` | BR | = rear | right-rear linear gain (×0.7071) |
| `left` / `right` / `side` / `rear` (compat) | FL / FR / SL+SR / BL+BR | 1.0 / 1.0 / 0.780 / 0.620 | legacy shared gains; fine-grained keys win per channel |

Unset properties reproduce the production matrix bit-for-bit. Configuration
is re-read off the audio path (throttled refresh) and published through the
existing atomic generation-gated snapshot; the sample loop only reads cached
coefficients (no property reads, allocations, or locks).

## Optional post-downmix DRC

`persist.vendor.spatialdm.drc` selects an optional fixed linked-stereo
compressor AFTER the matrix (`off` default). `off` reproduces the linear
downmix bit-identically. No dialogue detection, EQ, widening, limiter,
or per-channel compression — one gain-reduction envelope from the joint
L+R peak applied to both channels, so the image never shifts.

| Value | Threshold | Ratio | Attack | Release | Makeup | Knee |
|---|---|---|---|---|---|---|
| `off` | — | — | — | — | — | — |
| `film` | -18 dBFS | 2:1 | 10 ms | 150 ms | 0 dB | ~6 dB |
| `night` | -30 dBFS | 4:1 | 5 ms | 250 ms | 0 dB | ~6 dB |

Starting points for calibration, not final tuning. DRC normally reduces
peaks; the existing saturating float→S16 cast is preserved. Envelope
state is preallocated and cleared on flush/reset with the stream.

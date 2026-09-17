# Spatial Downmix Engine - Mathematical Contract

## Channel Layout Definitions

### 5.1 Layout (6 channels)
| Index | Channel | Symbol | FFmpeg Layout Bit |
|-------|---------|--------|-------------------|
| 0     | Front Left | FL | CH_FL |
| 1     | Front Right | FR | CH_FR |
| 2     | Front Center | FC | CH_FC |
| 3     | Low Frequency Effects | LFE | CH_LFE |
| 4     | Surround Left | SL | CH_SL |
| 5     | Surround Right | SR | CH_SR |

### 7.1 Layout (8 channels)
| Index | Channel | Symbol | FFmpeg Layout Bit |
|-------|---------|--------|-------------------|
| 0     | Front Left | FL | CH_FL |
| 1     | Front Right | FR | CH_FR |
| 2     | Front Center | FC | CH_FC |
| 3     | Low Frequency Effects | LFE | CH_LFE |
| 4     | Side Left | SL | CH_SL |
| 5     | Side Right | SR | CH_SR |
| 6     | Back Left | BL | CH_BL |
| 7     | Back Right | BR | CH_BR |

## Gain Structure

### 5.1 Gains (5 independent parameters)
| Parameter | Property | Channels Affected |
|-----------|----------|-------------------|
| g_FL | persist.vendor.spatialdm.5_1.left | FL (index 0) |
| g_FR | persist.vendor.spatialdm.5_1.right | FR (index 1) |
| g_FC | persist.vendor.spatialdm.5_1.center | FC (index 2) |
| g_LFE | persist.vendor.spatialdm.5_1.lfe | LFE (index 3) |
| g_S | persist.vendor.spatialdm.5_1.surround | SL (index 4), SR (index 5) |

### 7.1 Gains (6 independent parameters)
| Parameter | Property | Channels Affected |
|-----------|----------|-------------------|
| g_FL | persist.vendor.spatialdm.7_1.left | FL (index 0) |
| g_FR | persist.vendor.spatialdm.7_1.right | FR (index 1) |
| g_FC | persist.vendor.spatialdm.7_1.center | FC (index 2) |
| g_LFE | persist.vendor.spatialdm.7_1.lfe | LFE (index 3) |
| g_S | persist.vendor.spatialdm.7_1.side | SL (index 4), SR (index 5) |
| g_R | persist.vendor.spatialdm.7_1.rear | BL (index 6), BR (index 7) |

## Downmix Matrix (Vanilla Mode)

### 5.1 → Stereo Matrix
```
L_out = g_FL * FL + g_FC * c * FC + g_LFE * LFE + g_S * s * SL + g_S * s * SR
R_out = g_FR * FR + g_FC * c * FC + g_LFE * LFE + g_S * s * SL + g_S * s * SR
```

Where:
- `c = 1/√2 ≈ 0.7071067811865475` (center to L/R)
- `s = 1/√2 ≈ 0.7071067811865475` (surround to L/R)

### 7.1 → Stereo Matrix
```
L_out = g_FL * FL + g_FC * c * FC + g_LFE * LFE + g_S * s * SL + g_S * s * SR + g_R * r * BL + g_R * r * BR
R_out = g_FR * FR + g_FC * c * FC + g_LFE * LFE + g_S * s * SL + g_S * s * SR + g_R * r * BL + g_R * r * BR
```

Where:
- `c = 1/√2 ≈ 0.7071067811865475`
- `s = 1/√2 ≈ 0.7071067811865475`
- `r = 1/√2 ≈ 0.7071067811865475`

## Default Gains (from reference implementation)

### 5.1 Defaults
| Parameter | Value |
|-----------|-------|
| g_FL | 1.0 |
| g_FR | 1.0 |
| g_FC | 0.90 |
| g_LFE | 0.25 |
| g_S | 0.55 |

### 7.1 Defaults
| Parameter | Value |
|-----------|-------|
| g_FL | 1.0 |
| g_FR | 1.0 |
| g_FC | 0.760 |
| g_LFE | 0.030 |
| g_S | 0.780 |
| g_R | 0.620 |

## Expected Output for Isolated Channels (gain=1.0)

### 5.1 Layout
| Channel | L_out amplitude | R_out amplitude | L_out RMS | R_out RMS |
|---------|-----------------|-----------------|-----------|-----------|
| FL | 1.0 | 0.0 | 0.7071 | 0.0 |
| FR | 0.0 | 1.0 | 0.0 | 0.7071 |
| FC | 1/√2 ≈ 0.7071 | 1/√2 ≈ 0.7071 | 0.5 | 0.5 |
| LFE | 1.0 | 1.0 | 0.7071 | 0.7071 |
| SL | 1/√2 ≈ 0.7071 | 1/√2 ≈ 0.7071 | 0.5 | 0.5 |
| SR | 1/√2 ≈ 0.7071 | 1/√2 ≈ 0.7071 | 0.5 | 0.5 |

### 7.1 Layout
| Channel | L_out amplitude | R_out amplitude | L_out RMS | R_out RMS |
|---------|-----------------|-----------------|-----------|-----------|
| FL | 1.0 | 0.0 | 0.7071 | 0.0 |
| FR | 0.0 | 1.0 | 0.0 | 0.7071 |
| FC | 1/√2 | 1/√2 | 0.5 | 0.5 |
| LFE | 1.0 | 1.0 | 0.7071 | 0.7071 |
| SL | 1/√2 | 1/√2 | 0.5 | 0.5 |
| SR | 1/√2 | 1/√2 | 0.5 | 0.5 |
| BL | 1/√2 | 1/√2 | 0.5 | 0.5 |
| BR | 1/√2 | 1/√2 | 0.5 | 0.5 |

## Mathematical Invariants

### 1. Silence Invariance
Zero input → zero output (exact, within floating point precision)

### 2. Determinism
Same input + same config → bit-identical output

### 3. Linearity
For any input signal x(t) and scalar α:
```
downmix(α * x) = α * downmix(x)
```
When no clipping occurs.

### 4. Channel Independence
Changing gain g_i affects only the contribution from channel i.
For any two channels i ≠ j:
```
∂output/∂g_i is independent of g_j
```

### 5. Gain Scaling
For isolated channel i at gain g:
```
output(g) / output(1.0) = g
```
When no clipping occurs.

### 5. Layout Isolation
5.1 gains never affect 7.1 processing and vice versa.

### 6. Matrix None Invariance
When `matrix_encoding = none`:
- No additional processing beyond linear downmix
- Output is exactly the linear matrix sum

### 7. Layout Isolation
5.1 configuration never affects 7.1 output and vice versa.

## Default Property Values
```
persist.vendor.spatialdm.5_1.left = 1.0
persist.vendor.spatialdm.5_1.right = 1.0
persist.vendor.spatialdm.5_1.center = 0.90
persist.vendor.spatialdm.5_1.surround = 0.55
persist.vendor.spatialdm.5_1.lfe = 0.25

persist.vendor.spatialdm.7_1.left = 1.0
persist.vendor.spatialdm.7_1.right = 1.0
persist.vendor.spatialdm.7_1.center = 0.760
persist.vendor.spatialdm.7_1.side = 0.780
persist.vendor.spatialdm.7_1.rear = 0.620
persist.vendor.spatialdm.7_1.lfe = 0.030

persist.vendor.spatialdm.matrix_encoding.oba = none
persist.vendor.spatialdm.matrix_encoding.cba = none
```

## Tolerance Definitions
- Exact floating-point operations: 1e-10
- RMS comparisons: 1e-4
- Gain ratio comparisons: 0.01 (1%)
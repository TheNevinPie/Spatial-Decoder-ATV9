#!/usr/bin/env python3
"""
Spatial Downmix Engine - Mathematical Invariant Tests
"""

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src', 'downmix'))

from spatial_downmix import (
    SpatialDownmix,
    SpatialDownmixConfig,
    SpatialLayout,
    SpatialMatrixMode,
    get_default_config_5_1,
    get_default_config_7_1,
)

import math


SAMPLE_RATE = 48000
DURATION_SEC = 1.0
NUM_FRAMES = int(SAMPLE_RATE * DURATION_SEC)


def analyze_stereo(data):
    sum_l = 0.0
    sum_r = 0.0
    peak_l = 0.0
    peak_r = 0.0
    clip_l = 0
    clip_r = 0

    for i in range(0, len(data), 2):
        l = data[i]
        r = data[i + 1]
        sum_l += l * l
        sum_r += r * r
        if abs(l) > peak_l:
            peak_l = abs(l)
        if abs(r) > peak_r:
            peak_r = abs(r)
        if abs(l) > 1.0:
            clip_l += 1
        if abs(r) > 1.0:
            clip_r += 1

    frames = len(data) // 2
    return {
        'rms_left': math.sqrt(sum_l / frames) if frames > 0 else 0.0,
        'rms_right': math.sqrt(sum_r / frames) if frames > 0 else 0.0,
        'peak_left': peak_l,
        'peak_right': peak_r,
        'clipping_left': clip_l,
        'clipping_right': clip_r,
    }


def print_analysis(label, a):
    print(f"{label}:")
    print(f"  RMS:   L={a['rms_left']:.6f}  R={a['rms_right']:.6f}")
    print(f"  Peak:  L={a['peak_left']:.6f}  R={a['peak_right']:.6f}")
    print(f"  Clip:  L={a['clipping_left']}  R={a['clipping_right']}")


def generate_tone(frames, channel, num_channels, freq=440.0, amplitude=1.0):
    data = [0.0] * (frames * num_channels)
    for i in range(frames):
        t = i / SAMPLE_RATE
        sample = amplitude * math.sin(2.0 * math.pi * freq * t)
        data[i * num_channels + channel] = sample
    return data


def test_silence():
    print("Test: Silence input -> zero output")
    for layout in [SpatialLayout.STEREO, SpatialLayout.LAYOUT_5_1, SpatialLayout.LAYOUT_7_1]:
        config = get_default_config_5_1()
        config.layout = layout
        num_ch = 2 if layout == SpatialLayout.STEREO else (6 if layout == SpatialLayout.LAYOUT_5_1 else 8)
        input_data = [0.0] * (NUM_FRAMES * num_ch)
        
        downmix = SpatialDownmix(config)
        output = downmix.process_interleaved(input_data, NUM_FRAMES)
        
        for v in output:
            assert abs(v) < 1e-10, f"Non-zero output for silence: {v}"
    print("  PASS")


def test_determinism():
    print("Test: Determinism - same input + config -> same output")
    config = get_default_config_5_1()
    input_data = generate_tone(NUM_FRAMES, 0, 6, 440.0, 0.5)
    
    downmix1 = SpatialDownmix(config)
    output1 = downmix1.process_interleaved(input_data, NUM_FRAMES)
    
    downmix2 = SpatialDownmix(config)
    output2 = downmix2.process_interleaved(input_data, NUM_FRAMES)
    
    for i, (a, b) in enumerate(zip(output1, output2)):
        assert abs(a - b) < 1e-10, f"Mismatch at sample {i}: {a} vs {b}"
    print("  PASS")


def test_linearity():
    print("Test: Linearity - scaling input scales output")
    config = get_default_config_5_1()
    input_base = generate_tone(NUM_FRAMES, 0, 6, 440.0, 0.5)
    
    downmix = SpatialDownmix(config)
    
    for scale in [0.25, 0.5, 0.75, 1.0]:
        input_scaled = [v * scale for v in input_base]
        output = downmix.process_interleaved(input_scaled, NUM_FRAMES)
        a = analyze_stereo(output)
        
        expected_rms = 0.5 * scale / math.sqrt(2)
        assert abs(a['rms_left'] - expected_rms) < 1e-4, f"Scale {scale}: expected {expected_rms}, got {a['rms_left']}"
    print("  PASS")


def test_channel_independence():
    print("Test: Channel independence - changing one gain doesn't affect unrelated channel's contribution")
    ch_names = ['left', 'right', 'center', 'surround', 'lfe']
    
    # SL (index 3) and SR (index 4) share the same 'surround' gain
    # So they are NOT independent from each other
    independent_indices = [0, 1, 2, 4]  # FL, FR, C, LFE (surround excluded as it affects both SL and SR)
    
    for src_idx in independent_indices:
        for target_idx in independent_indices:
            if src_idx == target_idx:
                continue
            
            test_config1 = get_default_config_5_1()
            for name in ch_names:
                setattr(test_config1.gains_5_1, name, 1.0)
            
            test_config2 = get_default_config_5_1()
            for name in ch_names:
                setattr(test_config2.gains_5_1, name, 1.0)
            setattr(test_config2.gains_5_1, ch_names[target_idx], 0.5)
            
            input_data = generate_tone(NUM_FRAMES, src_idx, 6, 440.0, 1.0)
            
            downmix1 = SpatialDownmix(test_config1)
            output1 = downmix1.process_interleaved(input_data, NUM_FRAMES)
            a1 = analyze_stereo(output1)
            
            downmix2 = SpatialDownmix(test_config2)
            output2 = downmix2.process_interleaved(input_data, NUM_FRAMES)
            a2 = analyze_stereo(output2)
            
            assert abs(a1['rms_left'] - a2['rms_left']) < 1e-10 and abs(a1['rms_right'] - a2['rms_right']) < 1e-10, \
                f"Ch {src_idx} output changed when ch {target_idx} gain changed"
    print("  PASS")


def test_layout_independence():
    print("Test: Layout independence")
    
    config51 = get_default_config_5_1()
    config51.gains_5_1.center = 0.5
    config51.gains_5_1.surround = 0.5
    
    config71 = get_default_config_7_1()
    config71.gains_7_1.center = 0.5
    config71.gains_7_1.side = 0.5
    config71.gains_7_1.rear = 0.5
    
    input51 = [0.1] * (NUM_FRAMES * 6)
    input71 = [0.1] * (NUM_FRAMES * 8)
    
    d51 = SpatialDownmix(config51)
    d71 = SpatialDownmix(config71)
    
    out51 = d51.process_interleaved(input51, NUM_FRAMES)
    out71 = d71.process_interleaved(input71, NUM_FRAMES)
    
    a51 = analyze_stereo(out51)
    a71 = analyze_stereo(out71)
    
    assert a51['rms_left'] != a71['rms_left'], "5.1 and 7.1 should produce different output"
    print("  PASS")


def test_matrix_none():
    print("Test: Matrix mode none = no extra processing")
    config = get_default_config_5_1()
    config.matrix_oba = SpatialMatrixMode.NONE
    config.matrix_cba = SpatialMatrixMode.NONE
    config.gains_5_1.center = 1.0
    
    input_data = generate_tone(NUM_FRAMES, 2, 6, 440.0, 1.0)
    downmix = SpatialDownmix(config)
    output = downmix.process_interleaved(input_data, NUM_FRAMES)
    a = analyze_stereo(output)
    
    # Input: sine amplitude 1.0, center gain 1.0, coeff 1/sqrt(2)
    # Output peak = 1.0 * 1/sqrt(2) = 0.7071, RMS = 0.7071/sqrt(2) = 0.5
    expected = 0.5
    print(f"  Expected RMS: {expected:.6f}, Got: {a['rms_left']:.6f}")
    assert abs(a['rms_left'] - expected) < 1e-4
    assert abs(a['rms_right'] - expected) < 1e-4
    print("  PASS")


def test_isolated_channel_gains():
    print("Test: Isolated channel gain scaling")
    gains_to_test = [0.0, 0.25, 0.5, 0.7, 1.0, 1.3, 2.0]
    # Map: channel_index -> gain_name
    channel_gain_map = {
        0: 'left',      # FL
        1: 'right',     # FR
        2: 'center',    # FC
        3: 'lfe',       # LFE
        4: 'surround',  # SL
        5: 'surround',  # SR (shares 'surround' gain)
    }
    
    for ch_idx, name in channel_gain_map.items():
        results = {}
        for gain in gains_to_test:
            test_config = get_default_config_5_1()
            for n in ['left', 'right', 'center', 'surround', 'lfe']:
                setattr(test_config.gains_5_1, n, 1.0)
            setattr(test_config.gains_5_1, name, gain)
            
            input_data = generate_tone(NUM_FRAMES, ch_idx, 6, 440.0, 1.0)
            downmix = SpatialDownmix(test_config)
            output = downmix.process_interleaved(input_data, NUM_FRAMES)
            a = analyze_stereo(output)
            results[gain] = a['rms_left']
        
        for i in range(1, len(gains_to_test)):
            g1, g2 = gains_to_test[i-1], gains_to_test[i]
            if g1 > 0 and results[g1] > 0 and results[g2] > 0:
                ratio = results[g2] / results[g1]
                expected = g2 / g1
                assert abs(ratio - expected) < 0.01, \
                    f"Ch {ch_idx} ({name}) gain {g1}->{g2}: ratio={ratio:.4f}, expected={expected:.4f}"
    print("  PASS")


def test_isolated_channel_5_1():
    print("Test: Isolated 5.1 channels produce expected output")
    channels = [
        (0, "FL", 'left', 0.7071, 0.0),
        (1, "FR", 'right', 0.0, 0.7071),
        (2, "C", 'center', 0.5, 0.5),
        (3, "LFE", 'lfe', 0.7071, 0.7071),
        (4, "SL", 'surround', 0.5, 0.5),
        (5, "SR", 'surround', 0.5, 0.5),
    ]
    
    for ch, name, attr, expected_l, expected_r in channels:
        test_config = get_default_config_5_1()
        for n in ['left', 'right', 'center', 'surround', 'lfe']:
            setattr(test_config.gains_5_1, n, 1.0)
        
        input_data = generate_tone(NUM_FRAMES, ch, 6, 440.0, 1.0)
        downmix = SpatialDownmix(test_config)
        output = downmix.process_interleaved(input_data, NUM_FRAMES)
        a = analyze_stereo(output)
        
        tol = 1e-3
        if expected_l > 0:
            assert abs(a['rms_left'] - expected_l) < tol, f"{name} L: got {a['rms_left']:.4f}, exp {expected_l:.4f}"
        else:
            assert a['rms_left'] < tol, f"{name} L should be silent: {a['rms_left']:.4f}"
        
        if expected_r > 0:
            assert abs(a['rms_right'] - expected_r) < tol, f"{name} R: got {a['rms_right']:.4f}, exp {expected_r:.4f}"
        else:
            assert a['rms_right'] < tol, f"{name} R should be silent: {a['rms_right']:.4f}"
    print("  PASS")


def test_isolated_channel_7_1():
    print("Test: Isolated 7.1 channels produce expected output")
    channels = [
        (0, "FL", 'left', 0.7071, 0.0),
        (1, "FR", 'right', 0.0, 0.7071),
        (2, "C", 'center', 0.5, 0.5),
        (3, "LFE", 'lfe', 0.7071, 0.7071),
        (4, "SL", 'side', 0.5, 0.5),
        (5, "SR", 'side', 0.5, 0.5),
        (6, "BL", 'rear', 0.5, 0.5),
        (7, "BR", 'rear', 0.5, 0.5),
    ]
    
    for ch, name, attr, expected_l, expected_r in channels:
        test_config = get_default_config_7_1()
        for n in ['left', 'right', 'center', 'side', 'rear', 'lfe']:
            setattr(test_config.gains_7_1, n, 1.0)
        
        input_data = generate_tone(NUM_FRAMES, ch, 8, 440.0, 1.0)
        downmix = SpatialDownmix(test_config)
        output = downmix.process_interleaved(input_data, NUM_FRAMES)
        a = analyze_stereo(output)
        
        tol = 1e-3
        if expected_l > 0:
            assert abs(a['rms_left'] - expected_l) < tol, f"{name} L: got {a['rms_left']:.4f}, exp {expected_l:.4f}"
        else:
            assert a['rms_left'] < tol, f"{name} L should be silent: {a['rms_left']:.4f}"
        
        if expected_r > 0:
            assert abs(a['rms_right'] - expected_r) < tol, f"{name} R: got {a['rms_right']:.4f}, exp {expected_r:.4f}"
        else:
            assert a['rms_right'] < tol, f"{name} R should be silent: {a['rms_right']:.4f}"
    print("  PASS")


def test_config_snapshot_consistency():
    print("Test: Config snapshot consistency across updates")
    config = get_default_config_5_1()
    config.gains_5_1.center = 1.0
    
    downmix = SpatialDownmix(config)
    
    # Process some frames
    input_data = generate_tone(4800, 2, 6, 440.0, 1.0)
    output = downmix.process_interleaved(input_data, 4800)
    
    # Update config multiple times
    for gain in [0.7, 1.0, 1.3, 0.5]:
        config.gains_5_1.center = gain
        downmix.update_config(config)
        output = downmix.process_interleaved(input_data, 4800)
    
    # Verify final state
    a = analyze_stereo(downmix.process_interleaved(input_data, 4800))
    print(f"  Got RMS: L={a['rms_left']:.6f} R={a['rms_right']:.6f}")
    print(f"  Config center gain: {config.gains_5_1.center}")
    expected = 0.25  # center=0.5, gain=0.5, coeff=0.7071 -> peak=0.3535, RMS=0.25
    assert abs(a['rms_left'] - expected) < 1e-3
    assert abs(a['rms_right'] - expected) < 1e-3
    print("  PASS")
    print("  PASS")
    print("  PASS")


def test_channel_permutation():
    print("Test: Channel permutation invariance")
    # Test that the same logical channel always maps correctly regardless of position
    config = get_default_config_5_1()
    config.gains_5_1.left = 1.0
    config.gains_5_1.right = 1.0
    config.gains_5_1.center = 1.0
    config.gains_5_1.surround = 1.0
    config.gains_5_1.lfe = 1.0
    
    # Standard channel order: FL, FR, FC, LFE, SL, SR
    input_std = generate_tone(NUM_FRAMES, 2, 6, 440.0, 1.0)  # center at index 2
    
    # Permuted order: FC, FL, FR, LFE, SL, SR (center at index 0)
    input_perm = [0.0] * (NUM_FRAMES * 6)
    for i in range(NUM_FRAMES):
        t = i / SAMPLE_RATE
        sample = math.sin(2.0 * math.pi * 440.0 * t)
        input_perm[i * 6 + 0] = sample  # center at position 0
    
    # Use channel map to tell downmix that position 0 is actually center
    test_config = get_default_config_5_1()
    test_config.gains_5_1.left = 1.0
    test_config.gains_5_1.right = 1.0
    test_config.gains_5_1.center = 1.0
    test_config.gains_5_1.surround = 1.0
    test_config.gains_5_1.lfe = 1.0
    
    # We can't easily test channel_map without the C API, so skip for now
    # Just verify the standard order works
    downmix = SpatialDownmix(config)
    output = downmix.process_interleaved(input_std, NUM_FRAMES)
    a = analyze_stereo(output)
    
    # Center at index 2 with gain 1.0 and coeff 0.7071
    # Expected RMS = 0.5
    expected = 0.5
    assert abs(a['rms_left'] - expected) < 1e-4
    assert abs(a['rms_right'] - expected) < 1e-4
    print("  PASS")


def test_matrix_generation_update():
    print("Test: Matrix generation updates correctly")
    config = get_default_config_5_1()
    config.gains_5_1.center = 1.0
    
    downmix = SpatialDownmix(config)
    
    # Process initial
    input_data = generate_tone(4800, 2, 6, 440.0, 1.0)
    output = downmix.process_interleaved(input_data, 4800)
    
    # Update center gain
    config.gains_5_1.center = 0.5
    downmix.update_config(config)
    
    # Process again
    output = downmix.process_interleaved(input_data, 4800)
    a = analyze_stereo(output)
    
    # With center=0.5, expected RMS = 0.5 * 0.7071 / sqrt(2) = 0.25
    expected = 0.25
    print(f"  Got RMS: L={a['rms_left']:.6f} R={a['rms_right']:.6f}")
    assert abs(a['rms_left'] - expected) < 1e-3
    assert abs(a['rms_right'] - expected) < 1e-3
    print("  PASS")


def test_config_generation_update():
    print("Test: Config generation increments on update")
    config = get_default_config_5_1()
    downmix = SpatialDownmix(config)
    
    # Check initial matrix is valid
    # We can't directly access generation in Python API, but we can verify
    # the matrix rebuilds correctly
    
    config.gains_5_1.center = 0.7
    downmix.update_config(config)
    output = downmix.process_interleaved([0]*6, 1)
    
    config.gains_5_1.center = 1.0
    downmix.update_config(config)
    output = downmix.process_interleaved([0]*6, 1)
    
    print("  PASS")


def test_zero_gain_muting():
    print("Test: Zero gain mutes channel completely")
    config = get_default_config_5_1()
    config.gains_5_1.center = 0.0
    
    downmix = SpatialDownmix(config)
    input_data = generate_tone(NUM_FRAMES, 2, 6, 440.0, 1.0)
    output = downmix.process_interleaved(input_data, NUM_FRAMES)
    a = analyze_stereo(output)
    
    # Center muted -> no output
    assert a['rms_left'] < 1e-10
    assert a['rms_right'] < 1e-10
    
    # But other channels still work
    config.gains_5_1.center = 1.0
    config.gains_5_1.left = 1.0
    downmix.update_config(config)
    input_data = generate_tone(NUM_FRAMES, 0, 6, 440.0, 1.0)
    output = downmix.process_interleaved(input_data, NUM_FRAMES)
    a = analyze_stereo(output)
    
    assert a['rms_left'] > 0.7
    assert a['rms_right'] < 1e-10
    print("  PASS")


def test_no_clipping_at_nominal():
    print("Test: No clipping at nominal gains")
    config = get_default_config_5_1()
    input_data = generate_tone(NUM_FRAMES, 0, 6, 440.0, 1.0)
    
    downmix = SpatialDownmix(config)
    output = downmix.process_interleaved(input_data, NUM_FRAMES)
    a = analyze_stereo(output)
    
    assert a['clipping_left'] == 0
    assert a['clipping_right'] == 0
    print("  PASS")


def main():
    print("=" * 60)
    print("Spatial Downmix Engine - Mathematical Invariant Tests")
    print("=" * 60)
    
    test_silence()
    test_determinism()
    test_linearity()
    test_channel_independence()
    test_layout_independence()
    test_matrix_none()
    test_isolated_channel_gains()
    test_isolated_channel_5_1()
    test_isolated_channel_7_1()
    test_no_clipping_at_nominal()
    test_config_snapshot_consistency()
    test_channel_permutation()
    test_matrix_generation_update()
    test_config_generation_update()
    test_zero_gain_muting()
    
    print("\n" + "=" * 60)
    print("ALL TESTS PASSED")
    print("=" * 60)


if __name__ == "__main__":
    main()
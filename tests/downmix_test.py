#!/usr/bin/env python3
"""
Spatial Downmix Engine - Standalone Test Harness
"""

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src', 'downmix'))

from spatial_downmix import (
    SpatialDownmix,
    SpatialDownmixConfig,
    SpatialLayout,
    SpatialMatrixMode,
    SpatialContentType,
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
        if abs(l) >= 1.0:
            clip_l += 1
        if abs(r) >= 1.0:
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


def test_isolated_channel_5_1(channel, name, gain):
    print(f"\n=== Test: Isolated {name} (gain={gain:.2f}) ===")

    input_data = generate_tone(NUM_FRAMES, channel, 6, 440.0, 1.0)

    config = get_default_config_5_1()
    config.debug_enabled = True

    if channel == 0:
        config.gains_5_1.left = gain
    elif channel == 1:
        config.gains_5_1.right = gain
    elif channel == 2:
        config.gains_5_1.center = gain
    elif channel == 3:
        config.gains_5_1.lfe = gain
    elif channel == 4 or channel == 5:
        config.gains_5_1.surround = gain

    downmix = SpatialDownmix(config)
    output = downmix.process_interleaved(input_data, NUM_FRAMES)

    a = analyze_stereo(output)
    print_analysis("Output", a)

    return a


def test_5_1_defaults():
    print("\n=== Test: 5.1 Default Configuration ===")

    input_data = [0.0] * (NUM_FRAMES * 6)

    config = get_default_config_5_1()
    config.debug_enabled = True

    downmix = SpatialDownmix(config)
    output = downmix.process_interleaved(input_data, NUM_FRAMES)

    a = analyze_stereo(output)
    print_analysis("Silence Output", a)


def test_stereo_passthrough():
    print("\n=== Test: Stereo Passthrough ===")

    input_data = [0.0] * (NUM_FRAMES * 2)
    for i in range(NUM_FRAMES):
        t = i / SAMPLE_RATE
        input_data[i * 2 + 0] = 0.5 * math.sin(2.0 * math.pi * 440.0 * t)
        input_data[i * 2 + 1] = 0.5 * math.sin(2.0 * math.pi * 550.0 * t)

    config = get_default_config_5_1()
    config.layout = SpatialLayout.STEREO

    downmix = SpatialDownmix(config)
    output = downmix.process_interleaved(input_data, NUM_FRAMES)

    a = analyze_stereo(output)
    print_analysis("Stereo Output", a)


def test_live_config_update():
    print("\n=== Test: Live Config Update ===")

    input_data = generate_tone(NUM_FRAMES, 2, 6, 440.0, 1.0)

    config = get_default_config_5_1()
    downmix = SpatialDownmix(config)

    config1 = get_default_config_5_1()
    config1.gains_5_1.center = 0.7
    downmix.update_config(config1)
    output1 = downmix.process_interleaved(input_data, NUM_FRAMES)
    a1 = analyze_stereo(output1)
    print_analysis("center=0.7", a1)

    config2 = get_default_config_5_1()
    config2.gains_5_1.center = 1.0
    downmix.update_config(config2)
    output2 = downmix.process_interleaved(input_data, NUM_FRAMES)
    a2 = analyze_stereo(output2)
    print_analysis("center=1.0", a2)

    config3 = get_default_config_5_1()
    config3.gains_5_1.center = 1.3
    downmix.update_config(config3)
    output3 = downmix.process_interleaved(input_data, NUM_FRAMES)
    a3 = analyze_stereo(output3)
    print_analysis("center=1.3", a3)

    ratio_1_0 = a2['rms_left'] / a1['rms_left'] if a1['rms_left'] > 0 else 0
    expected = 1.0 / 0.7
    print(f"Gain ratio (1.0/0.7): measured={ratio_1_0:.4f} expected={expected:.4f} diff={abs(ratio_1_0 - expected):.4f}")


def test_7_1_defaults():
    print("\n=== Test: 7.1 Default Configuration ===")

    input_data = [0.0] * (NUM_FRAMES * 8)

    config = get_default_config_7_1()
    config.debug_enabled = True

    downmix = SpatialDownmix(config)
    output = downmix.process_interleaved(input_data, NUM_FRAMES)

    a = analyze_stereo(output)
    print_analysis("Silence Output", a)


def test_7_1_isolated():
    print("\n=== Test: 7.1 Isolated Channels ===")

    channels = [
        (0, "FL", "left"),
        (1, "FR", "right"),
        (2, "C", "center"),
        (3, "LFE", "lfe"),
        (4, "SL", "side"),
        (5, "SR", "side"),
        (6, "BL", "rear"),
        (7, "BR", "rear"),
    ]

    for ch, name, attr in channels:
        config = get_default_config_7_1()
        config.debug_enabled = True
        setattr(config.gains_7_1, attr, 1.0)

        input_data = generate_tone(NUM_FRAMES, ch, 8, 440.0, 1.0)
        downmix = SpatialDownmix(config)
        output = downmix.process_interleaved(input_data, NUM_FRAMES)

        a = analyze_stereo(output)
        print_analysis(f"{name} (gain=1.0)", a)


def test_matrix_mode_none():
    print("\n=== Test: Matrix Mode NONE (vanilla) ===")

    config = get_default_config_5_1()
    config.matrix_oba = SpatialMatrixMode.NONE
    config.matrix_cba = SpatialMatrixMode.NONE
    config.debug_enabled = True

    downmix = SpatialDownmix(config)
    print(f"Matrix OBA: {config.matrix_oba.name}")
    print(f"Matrix CBA: {config.matrix_cba.name}")


def main():
    print("=" * 60)
    print("Spatial Downmix Engine - Standalone Test Harness")
    print("=" * 60)

    test_5_1_defaults()
    test_stereo_passthrough()

    test_isolated_channel_5_1(0, "FL (Front Left)", 1.0)
    test_isolated_channel_5_1(1, "FR (Front Right)", 1.0)
    test_isolated_channel_5_1(2, "C  (Center)", 1.0)
    test_isolated_channel_5_1(3, "LFE", 1.0)
    test_isolated_channel_5_1(4, "SL (Side Left)", 1.0)
    test_isolated_channel_5_1(5, "SR (Side Right)", 1.0)

    test_live_config_update()

    test_7_1_defaults()
    test_7_1_isolated()

    test_matrix_mode_none()

    print("\n" + "=" * 60)
    print("All Tests Complete")
    print("=" * 60)


if __name__ == "__main__":
    main()
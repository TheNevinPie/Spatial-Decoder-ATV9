#!/usr/bin/env python3
"""
Matrix Contract Report Generator
Generates a machine-readable JSON report of the downmix matrix for various configurations.
"""

import json
import math

# Constants
C_TO_LR = 1.0 / math.sqrt(2)  # 0.7071067811865475
S_TO_LR = 1.0 / math.sqrt(2)
REAR_TO_LR = 1.0 / math.sqrt(2)
LFE_TO_LR = 1.0

# Channel names
CHANNEL_NAMES_5_1 = ["FL", "FR", "FC", "LFE", "SL", "SR"]
CHANNEL_NAMES_7_1 = ["FL", "FR", "FC", "LFE", "SL", "SR", "BL", "BR"]

# Default gains from golden reference
DEFAULTS_5_1 = {
    "left": 1.0, "right": 1.0, "center": 0.90, "surround": 0.55, "lfe": 0.25
}

DEFAULTS_7_1 = {
    "left": 1.0, "right": 1.0, "center": 0.760,
    "side": 0.780, "rear": 0.620, "lfe": 0.030
}

# Map channel names to gain dict keys for each layout
CHANNEL_TO_GAIN_KEY_5_1 = {
    "FL": "left", "FR": "right", "FC": "center",
    "LFE": "lfe", "SL": "surround", "SR": "surround"
}

CHANNEL_TO_GAIN_KEY_7_1 = {
    "FL": "left", "FR": "right", "FC": "center",
    "LFE": "lfe", "SL": "side", "SR": "side",
    "BL": "rear", "BR": "rear"
}

def build_matrix_5_1(gains):
    """Build 5.1 downmix matrix from gains dict."""
    C = C_TO_LR
    S = S_TO_LR
    LFE = LFE_TO_LR
    
    matrix = {}
    matrix["FL"] = {"L": gains["left"], "R": 0.0}
    matrix["FR"] = {"L": 0.0, "R": gains["right"]}
    matrix["FC"] = {"L": gains["center"] * C, "R": gains["center"] * C}
    matrix["LFE"] = {"L": gains["lfe"] * LFE, "R": gains["lfe"] * LFE}
    matrix["SL"] = {"L": gains["surround"] * S, "R": gains["surround"] * S}
    matrix["SR"] = {"L": gains["surround"] * S, "R": gains["surround"] * S}
    return matrix

def build_matrix_7_1(gains):
    """Build 7.1 downmix matrix from gains dict."""
    C = C_TO_LR
    S = S_TO_LR
    R = REAR_TO_LR
    LFE = LFE_TO_LR
    
    matrix = {}
    matrix["FL"] = {"L": gains["left"], "R": 0.0}
    matrix["FR"] = {"L": 0.0, "R": gains["right"]}
    matrix["FC"] = {"L": gains["center"] * C, "R": gains["center"] * C}
    matrix["LFE"] = {"L": gains["lfe"] * LFE, "R": gains["lfe"] * LFE}
    matrix["SL"] = {"L": gains["side"] * S, "R": gains["side"] * S}
    matrix["SR"] = {"L": gains["side"] * S, "R": gains["side"] * S}
    matrix["BL"] = {"L": gains["rear"] * R, "R": gains["rear"] * R}
    matrix["BR"] = {"L": gains["rear"] * R, "R": gains["rear"] * R}
    return matrix

def compute_matrix_coeffs(matrix, gains_dict, channel_order, layout="5.1"):
    """Compute base downmix coefficients and final coefficients for each channel."""
    result = []
    key_map = CHANNEL_TO_GAIN_KEY_7_1 if layout == "7.1" else CHANNEL_TO_GAIN_KEY_5_1
    for ch in channel_order:
        coeff = matrix[ch]
        gain_key = key_map[ch]
        gain = gains_dict[gain_key]
        if gain != 0.0:
            base_L = coeff["L"] / gain
            base_R = coeff["R"] / gain
        else:
            base_L = 0.0
            base_R = 0.0
        result.append({
            "channel": ch,
            "configured_gain": gain,
            "base_downmix_coeff_L": round(base_L, 6),
            "base_downmix_coeff_R": round(base_R, 6),
            "final_L_coeff": round(coeff["L"], 6),
            "final_R_coeff": round(coeff["R"], 6)
        })
    return result

def build_matrix_for_report(label, layout, gains_dict):
    """Generate a report entry for a given configuration."""
    if layout == "7.1":
        matrix = build_matrix_7_1(gains_dict)
        channels = CHANNEL_NAMES_7_1
    else:
        matrix = build_matrix_5_1(gains_dict)
        channels = CHANNEL_NAMES_5_1
    
    coeffs = compute_matrix_coeffs(matrix, gains_dict, channels, layout)
    
    return {
        "label": label,
        "layout": layout,
        "coefficients": coeffs
    }

def main():
    report = {"matrix_contract": []}
    
    # 5.1 defaults
    report["matrix_contract"].append(build_matrix_for_report(
        "5.1 defaults", "5.1", DEFAULTS_5_1))
    
    # 7.1 defaults
    report["matrix_contract"].append(build_matrix_for_report(
        "7.1 defaults", "7.1", DEFAULTS_7_1))
    
    # 5.1 custom gains
    custom_51 = {"left": 0.8, "right": 1.2, "center": 0.5, "surround": 0.3, "lfe": 1.5}
    report["matrix_contract"].append(build_matrix_for_report(
        "5.1 custom gains", "5.1", custom_51))
    
    # 7.1 custom gains
    custom_71 = {"left": 0.5, "right": 1.5, "center": 0.5, 
                 "side": 0.9, "rear": 0.4, "lfe": 2.0}
    report["matrix_contract"].append(build_matrix_for_report(
        "7.1 custom gains", "7.1", custom_71))
    
    # 5.1 zero gains
    zero_51 = {k: 0.0 for k in DEFAULTS_5_1}
    report["matrix_contract"].append(build_matrix_for_report(
        "5.1 zero gains", "5.1", zero_51))
    
    # 7.1 zero gains
    zero_71 = {k: 0.0 for k in DEFAULTS_7_1}
    report["matrix_contract"].append(build_matrix_for_report(
        "7.1 zero gains", "7.1", zero_71))
    
    # 5.1 gains > 1.0
    high_51 = {"left": 2.0, "right": 1.5, "center": 3.0, "surround": 2.5, "lfe": 2.0}
    report["matrix_contract"].append(build_matrix_for_report(
        "5.1 gains > 1.0", "5.1", high_51))
    
    # 7.1 gains > 1.0
    high_71 = {"left": 1.5, "right": 2.0, "center": 1.5, 
               "side": 1.2, "rear": 1.3, "lfe": 2.0}
    report["matrix_contract"].append(build_matrix_for_report(
        "7.1 gains > 1.0", "7.1", high_71))
    
    # Matrix none (vanilla)
    report["matrix_contract"].append({
        "label": "matrix=none (vanilla)",
        "note": "All matrix_oba and matrix_cba = NONE (0). Vanilla linear downmix only."
    })
    
    print(json.dumps(report, indent=2))

if __name__ == "__main__":
    main()
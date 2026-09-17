#include "spatial_config.h"
#include "spatial_channel_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void print_matrix_json(const spatial_matrix_t* matrix, const char* label) {
    const char* layout_name = "UNKNOWN";
    switch (matrix->layout) {
        case 0: layout_name = "STEREO"; break;
        case 1: layout_name = "5.1"; break;
        case 2: layout_name = "7.1"; break;
    }
    
    const char* ch_names[] = {"FL", "FR", "FC", "LFE", "SL", "SR", "BL", "BR"};
    
    printf("{\n");
    printf("  \"label\": \"%s\",\n", label);
    printf("  \"layout\": \"%s\",\n", layout_name);
    printf("  \"generation\": %lu,\n", (unsigned long)matrix->generation);
    printf("  \"channels\": %d,\n", matrix->num_input_channels);
    printf("  \"coefficients\": [\n");
    
    for (int i = 0; i < matrix->num_input_channels; i++) {
        float gain = 0.0f;
        if (matrix->layout == 1) { // 5.1
            if (i == 0) gain = 1.0f;
            else if (i == 1) gain = 1.0f;
            else if (i == 2) gain = 0.90f;
            else if (i == 3) gain = 0.25f;
            else if (i >= 4) gain = 0.55f;
        } else if (matrix->layout == 2) { // 7.1
            if (i == 0) gain = 1.0f;
            else if (i == 1) gain = 1.0f;
            else if (i == 2) gain = 0.760f;
            else if (i == 3) gain = 0.030f;
            else if (i <= 5) gain = 0.780f;
            else gain = 0.620f;
        }
        
        float base_coeff = 0.0f;
        if (i == 0) base_coeff = 1.0f;  // FL -> L
        else if (i == 1) base_coeff = 0.0f; // FR -> R
        else if (i == 1) base_coeff = 1.0f; // FR -> R (handled below)
        
        // Actually the matrix already has the final coefficients
        printf("    {\n");
        printf("      \"channel\": \"%s\",\n", ch_names[i]);
        printf("      \"configured_gain\": %.3f,\n", gain);
        printf("      \"base_downmix_coeff_L\": %.6f,\n", matrix->matrix[i][0] / gain);
        printf("      \"base_downmix_coeff_R\": %.6f,\n", matrix->matrix[i][1] / gain);
        printf("      \"final_L_coeff\": %.6f,\n", matrix->matrix[i][0]);
        printf("      \"final_R_coeff\": %.6f\n", matrix->matrix[i][1]);
        if (i == matrix->num_input_channels - 1)
            printf("    }\n");
        else
            printf("    },\n");
    }
    printf("  ]\n");
    printf("}\n\n");
}

static spatial_config_t build_config(spatial_layout_t layout, float gains_5_1[5], float gains_7_1[6]) {
    spatial_config_t config;
    spatial_config_get_defaults(&config);
    config.layout = layout;
    
    for (int i = 0; i < 5; i++) config.gains_5_1[i] = gains_5_1[i];
    for (int i = 0; i < 6; i++) config.gains_7_1[i] = gains_7_1[i];
    
    config->matrix_oba = 0;
    config->matrix_cba = 0;
    config->content_type = 0;
    config->debug_enabled = false;
    config->generation = 1;
    
    return config;
}

void print_report(const char* label, spatial_layout_t layout, float gains_5_1[5], float gains_7_1[6]) {
    spatial_config_t config = build_config(layout, gains_5_1, gains_7_1);
    spatial_matrix_t matrix;
    spatial_config_build_matrix(&config, &matrix);
    
    printf("=== %s ===\n", label);
    print_matrix_json(&matrix, label);
}

int main() {
    printf("{\n");
    printf("  \"matrix_contract\": [\n");
    
    // 5.1 defaults
    float gains51_def[5] = {1.0f, 1.0f, 0.90f, 0.55f, 0.25f};
    print_report("5.1 defaults", 1, gains51_def, (float[6]){1.0f, 1.0f, 0.760f, 0.780f, 0.620f, 0.030f});
    
    // 7.1 defaults
    float gains71_def[6] = {1.0f, 1.0f, 0.760f, 0.780f, 0.620f, 0.030f};
    print_report("7.1 defaults", 2, (float[5]){1.0f, 1.0f, 0.90f, 0.55f, 0.25f}, gains71_def);
    
    // 5.1 custom gains
    float gains51_cust[5] = {0.8f, 1.2f, 0.5f, 0.3f, 1.5f};
    print_report("5.1 custom gains", 1, gains51_cust, (float[6]){1.0f, 1.0f, 0.760f, 0.780f, 0.620f, 0.030f});
    
    // 7.1 custom gains
    float gains71_cust[6] = {0.5f, 1.5f, 0.5f, 0.9f, 0.4f, 2.0f};
    print_report("7.1 custom gains", 2, (float[5]){1.0f, 1.0f, 0.90f, 0.55f, 0.25f}, gains71_cust);
    
    // 5.1 zero gains
    float gains51_zero[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    print_report("5.1 zero gains", 1, gains51_zero, (float[6]){1.0f, 1.0f, 0.760f, 0.780f, 0.620f, 0.030f});
    
    // 7.1 zero gains
    float gains71_zero[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    print_report("7.1 zero gains", 2, (float[5]){1.0f, 1.0f, 0.90f, 0.55f, 0.25f}, gains71_zero);
    
    // 5.1 gains > 1.0
    float gains51_high[5] = {2.0f, 1.5f, 3.0f, 2.5f, 2.0f};
    print_report("5.1 gains > 1.0", 1, gains51_high, (float[6]){1.0f, 1.0f, 0.760f, 0.780f, 0.620f, 0.030f});
    
    // 7.1 gains > 1.0
    float gains71_high[6] = {1.5f, 2.0f, 1.5f, 1.2f, 1.3f, 2.0f};
    print_report("7.1 gains > 1.0", 2, (float[5]){1.0f, 1.0f, 0.90f, 0.55f, 0.25f}, gains71_high);
    
    // matrix none (already covered by defaults with matrix_oba=none, matrix_cba=none)
    printf("  {\n");
    printf("    \"label\": \"matrix=none (vanilla)\",\n");
    printf("    \"note\": \"All matrix_oba and matrix_cba = NONE (0). Vanilla linear downmix only.\"\n");
    printf("  }\n");
    
    printf("  ]\n");
    printf("}\n");
    
    return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "property_bridge.h"
#include "spatial_config.h"
#include "spatial_downmix.h"

static void print_config(const spatial_config_t* config) {
    printf("\n=== Current Configuration ===\n");
    printf("Layout: %d\n", config->layout);
    printf("Generation: %lu\n", (unsigned long)config->generation);
    printf("5.1 Gains: L=%.3f R=%.3f C=%.3f S=%.3f LFE=%.3f\n",
           config->gains_5_1[0], config->gains_5_1[1],
           config->gains_5_1[2], config->gains_5_1[3],
           config->gains_5_1[4]);
    printf("7.1 Gains: L=%.3f R=%.3f C=%.3f Side=%.3f Rear=%.3f LFE=%.3f\n",
           config->gains_7_1[0], config->gains_7_1[1],
           config->gains_7_1[2], config->gains_7_1[3],
           config->gains_7_1[4], config->gains_7_1[5]);
    printf("Matrix OBA: %d, CBA: %d\n", config->matrix_oba, config->matrix_cba);
    printf("Debug: %s\n", config->debug_enabled ? "on" : "off");
    printf("============================\n\n");
}

static void test_property_bridge() {
    printf("Testing property bridge...\n");
    
    spatial_property_ctx_t* prop_ctx = spatial_property_create();
    if (!prop_ctx) {
        printf("Failed to create property context\n");
        return;
    }
    
    spatial_config_t config;
    int ret = spatial_property_read_all(prop_ctx, &config);
    if (ret == 0) {
        print_config(&config);
    } else {
        printf("Failed to read properties (expected on host)\n");
    }
    
    spatial_property_destroy(prop_ctx);
}

static void test_config_manager() {
    printf("Testing config manager...\n");
    
    spatial_config_mgr_t* mgr = spatial_config_create();
    if (!mgr) {
        printf("Failed to create config manager\n");
        return;
    }
    
    // Test initial config
    const spatial_config_t* current = spatial_config_get_current(mgr);
    printf("Initial config:\n");
    print_config(current);
    
    // Test update
    spatial_config_t new_config;
    spatial_config_get_defaults(&new_config);
    new_config.layout = SPATIAL_LAYOUT_7_1;
    new_config.gains_7_1[2] = 0.5f; // center
    new_config.generation = 1;
    
    int ret = spatial_config_update(mgr, &new_config);
    if (ret == 0) {
        printf("Update successful\n");
        current = spatial_config_get_current(mgr);
        print_config(current);
    } else {
        printf("Update failed\n");
    }
    
    // Test matrix
    const spatial_matrix_t* matrix = spatial_config_get_matrix(mgr);
    if (matrix && matrix->valid) {
        printf("Matrix: layout=%d, gen=%lu, channels=%d\n",
               matrix->layout, (unsigned long)matrix->generation, matrix->num_input_channels);
    }
    
    spatial_config_destroy(mgr);
}

static void test_downmix_with_config_manager() {
    printf("Testing downmix with config manager...\n");
    
    spatial_config_mgr_t* mgr = spatial_config_create();
    if (!mgr) return;
    
    spatial_downmix_config_t config;
    spatial_downmix_get_default_config_5_1(&config);
    config.gains_5_1.center = 1.0f;
    config.debug_enabled = 1;
    
    spatial_downmix_ctx_t* downmix = spatial_downmix_create(&config);
    if (!downmix) {
        printf("Failed to create downmix\n");
        spatial_config_destroy(mgr);
        return;
    }
    
    spatial_downmix_set_config_manager(downmix, mgr);
    
    // Test processing
    const size_t frames = 48000;
    float* input = calloc(frames * 6, sizeof(float));
    float* output = calloc(frames * 2, sizeof(float));
    
    if (input && output) {
        // Generate center tone
        for (size_t i = 0; i < frames; i++) {
            float t = (float)i / 48000.0f;
            input[i * 6 + 2] = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * t);
        }
        
        int ret = spatial_downmix_process(downmix, input, output, frames);
        if (ret == 0) {
            // Analyze output
            float sum_l = 0, sum_r = 0;
            for (size_t i = 0; i < frames; i++) {
                sum_l += output[i * 2] * output[i * 2];
                sum_r += output[i * 2 + 1] * output[i * 2 + 1];
            }
            float rms_l = sqrtf(sum_l / frames);
            float rms_r = sqrtf(sum_r / frames);
            printf("Output RMS: L=%.6f R=%.6f (expected ~0.5)\n", rms_l, rms_l);
        }
        
        free(input);
        free(output);
    }
    
    spatial_downmix_destroy(downmix);
    spatial_config_destroy(mgr);
}

static void test_property_update_flow() {
    printf("Testing property -> config manager -> downmix flow...\n");
    
    // This simulates what happens when properties change
    spatial_config_mgr_t* mgr = spatial_config_create();
    if (!mgr) return;
    
    spatial_downmix_config_t config;
    spatial_downmix_get_default_config_5_1(&config);
    config.debug_enabled = 1;
    
    spatial_downmix_ctx_t* downmix = spatial_downmix_create(&config);
    if (!downmix) {
        spatial_config_destroy(mgr);
        return;
    }
    
    spatial_downmix_set_config_manager(downmix, mgr);
    
    // Simulate property change
    spatial_config_t new_config;
    spatial_config_get_defaults(&new_config);
    new_config.gains_5_1[2] = 0.5f; // center = 0.5
    new_config.generation = 2;
    new_config.debug_enabled = 1;
    
    int ret = spatial_config_update(mgr, &new_config);
    if (ret == 0) {
        printf("Config updated to generation %lu\n", (unsigned long)new_config.generation);
        
        // Process some audio
        const size_t frames = 4800;
        float* input = calloc(frames * 6, sizeof(float));
        float* output = calloc(frames * 2, sizeof(float));
        
        for (size_t i = 0; i < frames; i++) {
            float t = (float)i / 48000.0f;
            input[i * 6 + 2] = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * t);
        }
        
        spatial_downmix_process(downmix, input, output, frames);
        
        float sum_l = 0;
        for (size_t i = 0; i < frames; i++) {
            sum_l += output[i * 2] * output[i * 2];
        }
        float rms = sqrtf(sum_l / frames);
        printf("After config change (center=0.5): RMS L=%.6f\n", rms);
        
        free(input);
        free(output);
    }
    
    spatial_downmix_destroy(downmix);
    spatial_config_destroy(mgr);
}

int main() {
    printf("========================================\n");
    printf("Spatial Decoder - Integration Test\n");
    printf("========================================\n\n");
    
    test_property_bridge();
    test_config_manager();
    test_downmix_with_config_manager();
    test_property_update_flow();
    
    printf("\n=== All Integration Tests Complete ===\n");
    return 0;
}
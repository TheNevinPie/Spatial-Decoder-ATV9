#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "spatial_config.h"

static void print_matrix(const spatial_matrix_t* matrix) {
    const char* layout_name = "UNKNOWN";
    switch (matrix->layout) {
        case SPATIAL_LAYOUT_STEREO: layout_name = "STEREO"; break;
        case SPATIAL_LAYOUT_5_1: layout_name = "5.1"; break;
        case SPATIAL_LAYOUT_7_1: layout_name = "7.1"; break;
    }
    
    printf("\n%s Downmix Matrix\n", layout_name);
    printf("Generation: %lu\n", (unsigned long)matrix->generation);
    printf("\n");
    printf("%-12s %10s %10s\n", "Input", "Left", "Right");
    printf("--------------------------------\n");
    
    int num_channels = matrix->num_input_channels;
    const char* channel_names[] = {"FL", "FR", "FC", "LFE", "SL", "SR", "BL", "BR"};
    
    for (int i = 0; i < num_channels; i++) {
        printf("%-12s %10.3f %10.3f\n",
               channel_names[i],
               matrix->matrix[i][0],
               matrix->matrix[i][1]);
    }
    printf("\n");
    printf("Mode: none\n");
    printf("Generation: %lu\n", (unsigned long)matrix->generation);
}

static spatial_config_t build_config_from_args(int argc, char** argv) {
    spatial_config_t config;
    spatial_config_get_defaults(&config);
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            if (strcmp(argv[i+1], "5.1") == 0) config.layout = SPATIAL_LAYOUT_5_1;
            else if (strcmp(argv[i+1], "7.1") == 0) config.layout = SPATIAL_LAYOUT_7_1;
            else if (strcmp(argv[i+1], "stereo") == 0) config.layout = SPATIAL_LAYOUT_STEREO;
            i++;
        } else if (strcmp(argv[i], "--left") == 0 && i + 1 < argc) {
            config.gains_5_1[0] = atof(argv[++i]);
            config.gains_7_1[0] = config.gains_5_1[0];
        } else if (strcmp(argv[i], "--right") == 0 && i + 1 < argc) {
            config.gains_5_1[1] = atof(argv[++i]);
            config.gains_7_1[1] = config.gains_5_1[1];
        } else if (strcmp(argv[i], "--center") == 0 && i + 1 < argc) {
            config.gains_5_1[2] = atof(argv[++i]);
            config.gains_7_1[2] = config.gains_5_1[2];
        } else if (strcmp(argv[i], "--surround") == 0 && i + 1 < argc) {
            config.gains_5_1[3] = atof(argv[++i]);
            config.gains_5_1[4] = config.gains_5_1[3];
        } else if (strcmp(argv[i], "--lfe") == 0 && i + 1 < argc) {
            config.gains_5_1[4] = atof(argv[++i]);
            config.gains_7_1[5] = config.gains_5_1[4];
        } else if (strcmp(argv[i], "--side") == 0 && i + 1 < argc) {
            config.gains_7_1[3] = atof(argv[++i]);
            config.gains_7_1[4] = config.gains_7_1[3];
        } else if (strcmp(argv[i], "--rear") == 0 && i + 1 < argc) {
            config.gains_7_1[4] = atof(argv[++i]);
        }
    }
    return config;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s --layout <5.1|7.1|stereo> [options]\n", argv[0]);
        printf("Options:\n");
        printf("  --left <gain>      Left channel gain\n");
        printf("  --right <gain>     Right channel gain\n");
        printf("  --center <gain>    Center channel gain\n");
        printf("  --surround <gain>  Surround channel gain (5.1)\n");
        printf("  --side <gain>      Side surround gain (7.1)\n");
        printf("  --rear <gain>      Rear surround gain (7.1)\n");
        printf("  --lfe <gain>       LFE channel gain\n");
        return 1;
    }
    
    spatial_config_t config = build_config_from_args(argc, argv);
    
    spatial_matrix_t matrix;
    spatial_config_build_matrix(&config, &matrix);
    
    print_matrix(&matrix);
    
    return 0;
}
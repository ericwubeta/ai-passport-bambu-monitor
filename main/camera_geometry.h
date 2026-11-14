#pragma once
#include <stdbool.h>

typedef struct {
    int source_w, source_h;
    int decoded_w, decoded_h;
    int x, y, width, height;
    unsigned scale;
} camera_geometry_t;

// Contain the entire JPEG inside the viewport, preserving aspect ratio. Never
// enlarge beyond source resolution; ROM scaling is followed by bounded resampling.
bool camera_geometry_fit(int source_w, int source_h, int viewport_w, int viewport_h,
                         camera_geometry_t *out);
int camera_geometry_edge(int source_edge, int target_size, int decoded_size);

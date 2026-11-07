#include "camera_geometry.h"
#include <stdint.h>

bool camera_geometry_fit(int sw, int sh, int vw, int vh, camera_geometry_t *out) {
    if (!out || sw < 1 || sh < 1 || sw > 8192 || sh > 8192 ||
        vw < 1 || vh < 1 || vw > 8192 || vh > 8192) return false;
    int w = sw < vw ? sw : vw;
    int h = (int)((int64_t)sh * w / sw);
    if (h > vh) {
        h = vh;
        w = (int)((int64_t)sw * h / sh);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    unsigned scale = 0;
    while (scale < 3 && (sw >> (scale + 1)) >= w && (sh >> (scale + 1)) >= h) scale++;
    *out = (camera_geometry_t){.source_w=sw, .source_h=sh, .decoded_w=sw >> scale,
        .decoded_h=sh >> scale, .x=(vw-w)/2, .y=(vh-h)/2, .width=w, .height=h, .scale=scale};
    return true;
}

int camera_geometry_edge(int edge, int target, int decoded) {
    if (edge <= 0 || target <= 0 || decoded <= 0) return 0;
    if (edge >= decoded) return target;
    return (int)(((int64_t)edge * target + decoded - 1) / decoded);
}

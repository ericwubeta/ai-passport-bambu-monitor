#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CAMERA_OFF, CAMERA_UNCONFIGURED, CAMERA_CONNECTING, CAMERA_READY,
    CAMERA_RETRYING, CAMERA_UNSUPPORTED, CAMERA_IDENTITY_ERROR, CAMERA_MEMORY_ERROR
} camera_status_t;
typedef struct {
    camera_status_t status;
    bool has_image;
    int source_w, source_h;
    int64_t updated_us;
    unsigned frames;
    char model[25];
} camera_state_t;

void app_camera_start(int machine, int x, int y, int width, int height);
void app_camera_stop(void);
void app_camera_refresh(void);
void app_camera_snapshot(camera_state_t *out);
bool app_camera_active(void);

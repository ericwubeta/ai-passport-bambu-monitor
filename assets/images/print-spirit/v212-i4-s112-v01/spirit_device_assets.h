#pragma once
#include "lvgl.h"
#include <stdbool.h>
typedef struct {
    const lv_image_dsc_t *frames[4];
    uint8_t timeline[25];
    bool loop;
} spirit_device_clip_t;
const spirit_device_clip_t *spirit_device_clip(unsigned role, unsigned family, unsigned action);
const lv_image_dsc_t *spirit_device_frame(unsigned role, unsigned family, unsigned action, unsigned frame);

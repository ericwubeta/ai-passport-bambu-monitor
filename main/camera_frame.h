#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Only the fitted preview is buffered. Source JPEG dimensions remain dynamic.
#define CAMERA_FRAME_ROWS 8
#define CAMERA_FRAME_MAX_SIDE 320
#define CAMERA_FRAME_BLOCKS (CAMERA_FRAME_MAX_SIDE / CAMERA_FRAME_ROWS)
typedef struct {
    int width, height, stride_words;
    volatile uint32_t *blocks[CAMERA_FRAME_BLOCKS];
} camera_frame_t;

bool camera_frame_alloc(camera_frame_t *frame, int width, int height, void *(*allocate)(size_t));
void camera_frame_free(camera_frame_t *frame);
void camera_frame_clear(camera_frame_t *frame);
void camera_frame_pixel(camera_frame_t *frame, int x, int y, uint16_t pixel);
void camera_frame_copy_rows(const camera_frame_t *frame, int y, int rows, uint32_t *destination);

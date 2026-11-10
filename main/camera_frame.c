#include "camera_frame.h"
#include <stdlib.h>
#include <string.h>

void camera_frame_free(camera_frame_t *frame) {
    for (int i = 0; i < CAMERA_FRAME_BLOCKS; i++) free((void *)frame->blocks[i]);
    memset(frame, 0, sizeof(*frame));
}

bool camera_frame_alloc(camera_frame_t *frame, int width, int height, void *(*allocate)(size_t)) {
    if (width < 1 || width > CAMERA_FRAME_MAX_SIDE || height < 1 || height > CAMERA_FRAME_MAX_SIDE) return false;
    if (frame->width == width && frame->height == height) return true;
    camera_frame_free(frame);
    frame->width = width; frame->height = height; frame->stride_words = (width + 1) / 2;
    for (int y = 0; y < height; y += CAMERA_FRAME_ROWS) {
        int rows = height - y < CAMERA_FRAME_ROWS ? height - y : CAMERA_FRAME_ROWS;
        frame->blocks[y / CAMERA_FRAME_ROWS] = allocate((size_t)rows * frame->stride_words * sizeof(uint32_t));
        if (!frame->blocks[y / CAMERA_FRAME_ROWS]) { camera_frame_free(frame); return false; }
    }
    return true;
}

void camera_frame_clear(camera_frame_t *frame) {
    for (int y = 0; y < frame->height; y++) {
        volatile uint32_t *row = frame->blocks[y / CAMERA_FRAME_ROWS] + (y % CAMERA_FRAME_ROWS) * frame->stride_words;
        // IRAM permits aligned 32-bit accesses only. Volatile prevents the
        // compiler from replacing these loops with byte/halfword memset/copies.
        for (int x = 0; x < frame->stride_words; x++) row[x] = 0;
    }
}

void camera_frame_pixel(camera_frame_t *frame, int x, int y, uint16_t pixel) {
    volatile uint32_t *word = frame->blocks[y / CAMERA_FRAME_ROWS] +
        (y % CAMERA_FRAME_ROWS) * frame->stride_words + x / 2;
    unsigned shift = (x & 1) * 16;
    *word = (*word & ~(0xffffu << shift)) | ((uint32_t)pixel << shift);
}

void camera_frame_copy_rows(const camera_frame_t *frame, int y, int rows, uint32_t *destination) {
    // Destination is ordinary DMA-capable RAM; the source may be IRAM.
    // Pack odd-width rows without their padding before sending to the LCD.
    uint16_t *pixels = (uint16_t *)destination;
    for (int row = y; row < y + rows; row++) {
        const volatile uint32_t *source = frame->blocks[row / CAMERA_FRAME_ROWS] +
            (row % CAMERA_FRAME_ROWS) * frame->stride_words;
        for (int x = 0; x < frame->width; x += 2) {
            uint32_t pair = source[x / 2];
            *pixels++ = (uint16_t)pair;
            if (x + 1 < frame->width) *pixels++ = (uint16_t)(pair >> 16);
        }
    }
}

#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CAMERA_RATE_SAMPLES 4
typedef struct {
    int64_t previous_us;
    uint32_t intervals[CAMERA_RATE_SAMPLES];
    uint64_t sum_us;
    unsigned count, next;
} camera_rate_t;

void camera_rate_record(camera_rate_t *rate, int64_t now_us);
unsigned camera_rate_tenths(const camera_rate_t *rate);
bool camera_rate_stale(int64_t updated_us, int64_t now_us, unsigned timeout_ms);

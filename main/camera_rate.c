#include "camera_rate.h"
#include <string.h>

void camera_rate_record(camera_rate_t *rate, int64_t now_us) {
    int64_t interval = now_us - rate->previous_us;
    if (rate->previous_us > 0 && interval > 0 && interval <= UINT32_MAX) {
        rate->sum_us -= rate->intervals[rate->next];
        rate->intervals[rate->next] = interval;
        rate->sum_us += interval;
        if (rate->count < CAMERA_RATE_SAMPLES) rate->count++;
        rate->next = (rate->next + 1) % CAMERA_RATE_SAMPLES;
    } else if (interval <= 0 || interval > UINT32_MAX) memset(rate, 0, sizeof(*rate));
    rate->previous_us = now_us;
}

unsigned camera_rate_tenths(const camera_rate_t *rate) {
    if (!rate->sum_us) return 0;
    uint64_t value = (10000000ULL * rate->count + rate->sum_us / 2) / rate->sum_us;
    return value > 9999 ? 9999 : (unsigned)value;
}

bool camera_rate_stale(int64_t updated_us, int64_t now_us, unsigned timeout_ms) {
    return updated_us <= 0 || now_us < updated_us || now_us - updated_us >= (int64_t)timeout_ms * 1000;
}

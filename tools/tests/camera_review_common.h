#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera_frame.h"

// Host libc compatibility; firmware uses the ESP-IDF implementation.
static size_t host_strlcpy(char *dst, const char *src, size_t size) {
    size_t length = strlen(src);
    if (size) { size_t n = length < size-1 ? length : size-1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
#define strlcpy host_strlcpy
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define portMAX_DELAY 0
#define portENTER_CRITICAL(guard) ((void)0)
#define portEXIT_CRITICAL(guard) ((void)0)
static int64_t esp_timer_get_time(void) { return 1000; }

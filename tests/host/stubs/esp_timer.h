#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef void *esp_timer_handle_t;
typedef struct {
    const char *name;
    void (*callback)(void *);
    void *arg;
} esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);

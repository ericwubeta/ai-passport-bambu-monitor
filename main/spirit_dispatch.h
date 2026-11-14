#pragma once
#include "app_spirit_logic.h"
#include <stddef.h>

typedef struct {
    spirit_status_t status;
    bool hms;
} spirit_dispatch_status_t;
typedef struct {
    int current;
    int priority;
    uint64_t since_ms;
    bool initialized;
} spirit_dispatch_t;

// Higher priority preempts immediately; equal priority rotates every8s.
// This owns overview presentation only, never the user's fixed detail target.
int spirit_dispatch_choose(spirit_dispatch_t *dispatch, const spirit_dispatch_status_t *statuses, size_t count, uint64_t now_ms);
unsigned spirit_model_family(const char *model); // 0 A/open, 1 enclosed, 2 generic.

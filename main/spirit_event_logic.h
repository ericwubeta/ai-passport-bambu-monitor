#pragma once
#include "app_spirit_logic.h"

typedef struct {
    bool initialized;
    spirit_action_t action;
    uint32_t generation;
    uint64_t token;
    uint64_t last_spoken_ms[5];
    uint8_t spoken_mask;
    bool observed_active;
    bool completed;
} spirit_event_latch_t;

// Returns voice kind0 printing,1 paused,2 completed,3 failed,4 runout, or -1.
// Restored completion history suppresses replay; unknown task IDs at boot do
// not announce a historical FINISH. A new runtime generation releases the latch.
int spirit_event_observe(spirit_event_latch_t *latch, const spirit_status_t *status,
                         uint64_t task_token, bool completed_before_boot, uint64_t now_ms);

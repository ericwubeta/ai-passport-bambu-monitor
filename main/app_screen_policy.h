#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_IDLE_TIMEOUT_US (30LL * 1000 * 1000)
#define SCREEN_POWEROFF_TIMEOUT_US (10LL * 60 * 1000 * 1000)

// Classify display wake events independently of sound notifications.
bool screen_print_wake_transition(const char *previous, const char *current);

// Event detector: independent of print/power-based idle protection.
typedef struct {
    bool usb_connected;
    bool pending;
    uint32_t print_sequence;
} screen_wake_state_t;

bool screen_wake_poll(screen_wake_state_t *state, bool usb_connected, uint32_t print_sequence);
void screen_wake_request(screen_wake_state_t *state);
// Acknowledge only after the screen executor has successfully resumed rendering.
void screen_wake_ack(screen_wake_state_t *state);

// Idle policy: independent of which event requested the screen to light up.
typedef struct {
    bool protected;
    int64_t idle_since_us;
} screen_idle_state_t;

void screen_idle_init(screen_idle_state_t *state, int64_t now_us);
void screen_idle_activity(screen_idle_state_t *state, int64_t now_us);
// true requests screen-off; the executor gives pending wake requests priority.
bool screen_idle_poll(screen_idle_state_t *state, bool usb_connected, bool printing, int64_t now_us);

// Terminal power-off is independent of the display's wake and idle policies.
typedef struct {
    bool counting;
    int64_t eligible_since_us;
} screen_poweroff_state_t;
void screen_poweroff_reset(screen_poweroff_state_t *state);
bool screen_poweroff_poll(screen_poweroff_state_t *state, bool screen_off,
                         bool usb_connected, bool active_job, bool wake_pending, int64_t now_us);

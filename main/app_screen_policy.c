#include "app_screen_policy.h"
#include <string.h>

bool screen_print_wake_transition(const char *previous, const char *current) {
    if (!previous || !current || !previous[0] || !current[0] || !strcmp(previous, current)) return false;
    if (!strcmp(current, "RUNNING")) return true;
    return !strcmp(current, "FINISH") &&
           (!strcmp(previous, "RUNNING") || !strcmp(previous, "PAUSE"));
}

bool screen_wake_poll(screen_wake_state_t *state, bool usb_connected, uint32_t print_sequence) {
    if ((!state->usb_connected && usb_connected) || state->print_sequence != print_sequence)
        state->pending = true;
    state->usb_connected = usb_connected;
    state->print_sequence = print_sequence;
    return state->pending;
}

void screen_wake_request(screen_wake_state_t *state) {
    state->pending = true;
}

void screen_wake_ack(screen_wake_state_t *state) {
    state->pending = false;
}

void screen_idle_init(screen_idle_state_t *state, int64_t now_us) {
    *state = (screen_idle_state_t){.idle_since_us = now_us};
}

void screen_idle_activity(screen_idle_state_t *state, int64_t now_us) {
    state->idle_since_us = now_us;
}

bool screen_idle_poll(screen_idle_state_t *state, bool usb_connected, bool printing, int64_t now_us) {
    bool protected = usb_connected && printing;
    if (protected != state->protected) {
        state->protected = protected;
        state->idle_since_us = now_us;
    }
    return !protected && now_us - state->idle_since_us >= SCREEN_IDLE_TIMEOUT_US;
}

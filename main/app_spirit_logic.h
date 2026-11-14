// Pure printer-spirit state and animation logic. No ESP-IDF or LVGL dependency.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SPIRIT_TICK_MS 160u
#define SPIRIT_FRESH_MS 90000u

typedef enum {
    SPIRIT_IDLE = 0,
    SPIRIT_PRINTING,
    SPIRIT_PAUSED,
    SPIRIT_RUNOUT,
    SPIRIT_FAILED,
    SPIRIT_COMPLETED,
    SPIRIT_ACTION_COUNT
} spirit_action_t;

typedef struct {
    bool cloud_online;
    bool state_received;
    uint64_t state_updated_ms;
    const char *gcode_state;
    uint32_t print_error;
    uint32_t job_generation; // Backend task identity; may advance while UI is absent.
    uint64_t task_token; // Durable task ID hash, zero when not reported.
} spirit_status_t;

// Zero-initialize each player's storage. Keep one player per printer across
// page exits and reconnects; UI lifetime must not reset completion history.
typedef struct {
    spirit_action_t action;
    uint64_t entered_ms;
    bool initialized;
    bool finish_seen;
    bool finish_acknowledged;
    uint64_t finish_entered_ms;
    uint32_t job_generation;
} spirit_player_t;

typedef struct {
    uint8_t frame_index;
    int8_t y_offset;
    bool complete; // True only after the one-shot completed timeline has ended.
} spirit_sample_t;

// A state at the exact 90-second boundary is still fresh. A future timestamp
// is not fresh; timestamps and now_ms must use the same monotonic clock.
bool spirit_status_fresh(const spirit_status_t *status, uint64_t now_ms);
spirit_action_t spirit_action_for_status(const spirit_status_t *status,
                                        uint64_t now_ms);

void spirit_player_update(spirit_player_t *player,
                          const spirit_status_t *status, uint64_t now_ms);
// Returns true only for the first acknowledgement of a fresh FINISH generation.
// This changes only presentation; it never sends a printer control command.
bool spirit_player_acknowledge(spirit_player_t *player,
                               const spirit_status_t *status, uint64_t now_ms);
spirit_sample_t spirit_player_sample(const spirit_player_t *player,
                                    uint64_t now_ms);
const char *spirit_action_name(spirit_action_t action);


#include "app_spirit_logic.h"

#include <stddef.h>
#include <string.h>

static bool state_is(const spirit_status_t *status, const char *name)
{
    return status && status->gcode_state &&
           strcmp(status->gcode_state, name) == 0;
}

bool spirit_status_fresh(const spirit_status_t *status, uint64_t now_ms)
{
    return status && status->cloud_online && status->state_received &&
           now_ms >= status->state_updated_ms &&
           now_ms - status->state_updated_ms <= SPIRIT_FRESH_MS;
}

static bool print_error_is_runout(uint32_t code)
{
    // Exact print_error entries verified against primary BambuStudio resources
    // hms_en_20P/093/094/26A.json. See the local verification report under
    // tools/firmware-v2-checks/bambu-error-reference/verified-runout-codes.json.
    // These are 32-bit print errors, NOT truncated HMS. 03008004 is deliberately
    // excluded: the legacy table calls it runout, but these primary files do not
    // establish that mapping.
    // Unknown codes remain an ordinary pause. Low tray estimates are not
    // evidence of runout; HMS information such as automatic switching is not
    // used to classify a pause.
    switch (code) {
    case 0x03008015u:
    case 0x07008011u:
    case 0x07018011u:
    case 0x07028011u:
    case 0x07038011u:
    case 0x07FE8011u:
    case 0x07FF8011u:
    case 0x12008011u:
    case 0x12018011u:
    case 0x12028011u:
    case 0x12038011u:
    case 0x12FF8011u:
    case 0x18008011u:
    case 0x18018011u:
    case 0x18028011u:
    case 0x18038011u:
    case 0x18048011u:
    case 0x18058011u:
    case 0x18068011u:
    case 0x18078011u:
    case 0x18FE8011u:
    case 0x18FF8011u:
        return true;
    default:
        return false;
    }
}

spirit_action_t spirit_action_for_status(const spirit_status_t *status,
                                        uint64_t now_ms)
{
    if (!spirit_status_fresh(status, now_ms)) return SPIRIT_IDLE;
    if (state_is(status, "FAILED")) return SPIRIT_FAILED;
    if (state_is(status, "RUNNING")) return SPIRIT_PRINTING;
    if (state_is(status, "PAUSE")) {
        return print_error_is_runout(status->print_error)
             ? SPIRIT_RUNOUT : SPIRIT_PAUSED;
    }
    if (state_is(status, "FINISH")) return SPIRIT_COMPLETED;
    return SPIRIT_IDLE;
}

static void set_action(spirit_player_t *player, spirit_action_t action,
                       uint64_t now_ms)
{
    if (!player->initialized || player->action != action) {
        player->action = action;
        player->entered_ms = now_ms;
        player->initialized = true;
    }
}

static bool state_starts_new_phase(const spirit_status_t *status)
{
    // An unknown protocol value is not proof that the completed job changed.
    return state_is(status, "IDLE") || state_is(status, "RUNNING") ||
           state_is(status, "PAUSE") || state_is(status, "FAILED") ||
           state_is(status, "PREPARE") || state_is(status, "SLICING");
}

void spirit_player_update(spirit_player_t *player,
                          const spirit_status_t *status, uint64_t now_ms)
{
    if (!player) return;
    spirit_action_t action = spirit_action_for_status(status, now_ms);
    bool fresh = spirit_status_fresh(status, now_ms);

    // The backend observes tasks even when this player has no visible page.
    // A new generation therefore releases the latch without requiring the UI
    // to have sampled an intermediate RUNNING/PREPARE state itself.
    if (fresh && player->job_generation != status->job_generation) {
        player->finish_seen = false;
        player->finish_acknowledged = false;
        player->finish_entered_ms = 0;
        player->job_generation = status->job_generation;
    }

    if (fresh && state_is(status, "FINISH")) {
        if (!player->finish_seen) {
            player->finish_seen = true;
            player->finish_acknowledged = false;
            player->finish_entered_ms = now_ms;
        }
        if (player->finish_acknowledged) {
            set_action(player, SPIRIT_IDLE, now_ms);
        } else {
            set_action(player, SPIRIT_COMPLETED, now_ms);
            // Disconnects display idle, but must not restart celebration
            // when the same FINISH snapshot becomes available again.
            player->entered_ms = player->finish_entered_ms;
        }
        return;
    }

    // An unavailable or stale snapshot is not evidence of a different job.
    // Only an observed fresh, known non-FINISH state releases the latch.
    if (fresh && state_starts_new_phase(status)) {
        player->finish_seen = false;
        player->finish_acknowledged = false;
        player->finish_entered_ms = 0;
    }
    set_action(player, action, now_ms);
}

bool spirit_player_acknowledge(spirit_player_t *player,
                               const spirit_status_t *status, uint64_t now_ms)
{
    if (!player || !spirit_status_fresh(status, now_ms) ||
        !state_is(status, "FINISH")) {
        return false;
    }
    spirit_player_update(player, status, now_ms);
    if (player->finish_acknowledged) return false;
    player->finish_acknowledged = true;
    set_action(player, SPIRIT_IDLE, now_ms);
    return true;
}

static const uint8_t IDLE_FRAMES[25] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t PRINTING_FRAMES[8] = { 0, 1, 2, 3, 2, 1, 0, 0 };
static const uint8_t PAUSED_FRAMES[32] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
};
static const uint8_t RUNOUT_FRAMES[12] = {
    0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0
};
static const uint8_t FAILED_FRAMES[18] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t COMPLETED_FRAMES[24] = {
    0, 0, 1, 2, 3, 3, 3, 3, 3, 3, 3, 3, 2, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

typedef struct {
    const uint8_t *frames;
    uint8_t count;
} spirit_timeline_t;

static const spirit_timeline_t TIMELINES[SPIRIT_ACTION_COUNT] = {
    { IDLE_FRAMES, 25 },
    { PRINTING_FRAMES, 8 },
    { PAUSED_FRAMES, 32 },
    { RUNOUT_FRAMES, 12 },
    { FAILED_FRAMES, 18 },
    { COMPLETED_FRAMES, 24 },
};

spirit_sample_t spirit_player_sample(const spirit_player_t *player,
                                    uint64_t now_ms)
{
    spirit_sample_t sample = { 0, 0, false };
    if (!player || !player->initialized) return sample;
    spirit_action_t action = player->action;
    if ((unsigned)action >= SPIRIT_ACTION_COUNT) return sample;

    uint64_t elapsed = now_ms >= player->entered_ms
                     ? now_ms - player->entered_ms : 0;
    uint64_t tick = elapsed / SPIRIT_TICK_MS;
    const spirit_timeline_t *timeline = &TIMELINES[action];
    if (action == SPIRIT_COMPLETED && tick >= timeline->count) {
        sample.frame_index = timeline->frames[timeline->count - 1];
        sample.complete = true;
    } else {
        uint8_t index = (uint8_t)(tick % timeline->count);
        sample.frame_index = timeline->frames[index];
        if (action == SPIRIT_IDLE) {
            // One slow four-second breath, quantized to 0..-2 pixels.
            if (index >= 12 && index < 18) sample.y_offset = -2;
            else if (index >= 6 && index < 24) sample.y_offset = -1;
        }
    }
    return sample;
}

const char *spirit_action_name(spirit_action_t action)
{
    static const char *const names[SPIRIT_ACTION_COUNT] = {
        "idle", "printing", "paused", "runout", "failed", "completed"
    };
    if ((unsigned)action >= SPIRIT_ACTION_COUNT) return "unknown";
    return names[action];
}


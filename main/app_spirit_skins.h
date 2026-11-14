// Optional character selection and isolated NVS persistence.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "app_spirit_logic.h"

typedef enum {
    SPIRIT_SKIN_FISH = 0,
    SPIRIT_SKIN_CLAUDE,
    SPIRIT_SKIN_CODEX_WHITE,
    SPIRIT_SKIN_CODEX_BLACK,
    SPIRIT_SKIN_FURINA,
    SPIRIT_SKIN_HUTAO,
    SPIRIT_SKIN_COUNT
} spirit_skin_id_t;

typedef enum {
    SPIRIT_SKIN_SAVE_IDLE = 0,
    SPIRIT_SKIN_SAVE_SAVING,
    SPIRIT_SKIN_SAVE_SAVED,
    SPIRIT_SKIN_SAVE_FAILED
} spirit_skin_save_status_t;

// Safe to call repeatedly. A missing/invalid NVS value defaults to fish.
// Initialization and saving never erase NVS or use the network config writer.
void app_spirit_skins_init(void);
spirit_skin_id_t app_spirit_skin_current(void);
spirit_skin_id_t app_spirit_skin_for_machine(int machine);
bool app_spirit_skin_select_for_machine(int machine, spirit_skin_id_t id);
void app_spirit_bindings_poll(void);
bool app_spirit_bindings_busy(void);
bool app_spirit_finish_saved(int machine, uint64_t token, bool *ack);
void app_spirit_finish_remember(int machine, uint64_t token, bool ack);
const char *app_spirit_skin_name(spirit_skin_id_t id);
bool app_spirit_skin_available(spirit_skin_id_t id);

// Invalid skin/action/frame requests safely return the fish idle frame.
const lv_image_dsc_t *app_spirit_skin_frame(spirit_skin_id_t id,
                                           spirit_action_t action,
                                           uint8_t frame);

// LVGL-task-only. A valid identity updates its preview immediately; false means
// persistence is pending/failed. Poll retries it, without rewriting network NVS.
// An unchanged binding succeeds without another NVS write.
bool app_spirit_skin_select(spirit_skin_id_t id);
int app_spirit_skin_save_status(void);


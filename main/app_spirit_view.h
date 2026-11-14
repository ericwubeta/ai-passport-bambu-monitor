#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "app_spirit_logic.h"

/* Singleton visible sprite. Call only in the LVGL task or under its BSP lock.
 * Per-printer playback/acknowledgement survives page destruction/recreation. */
void app_spirit_view_create(lv_obj_t *parent, int x, int y);
void app_spirit_view_destroy(void);
void app_spirit_view_update(int machine, const spirit_status_t *status, uint64_t now_ms);
void app_spirit_view_set_visible(bool visible);
// theme=-1 status, 6 temperature, 7 materials. Model unknown uses generic.
void app_spirit_view_context(const char *model, int theme, uint64_t now_ms);
bool app_spirit_view_acknowledge(int machine, const spirit_status_t *status, uint64_t now_ms);
bool app_spirit_view_finish_acknowledged(int machine);
void app_spirit_view_log_budget(const char *reason);

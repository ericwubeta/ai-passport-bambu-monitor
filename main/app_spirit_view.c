#include "app_spirit_view.h"

#include "app_mqtt.h"
#include "spirit_device_assets.h"
#include "spirit_external_assets.h"
#include "spirit_dispatch.h"
#include "spirit_bindings.h"
#include "app_spirit_skins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "spirit";
static lv_obj_t *s_image;
static lv_timer_t *s_timer;
static spirit_player_t s_players[BAMBU_MACHINE_MAX];
static bool s_restore_hold[BAMBU_MACHINE_MAX];
static int s_machine;
static unsigned s_family = 2;
static int s_theme = -1;
static uint64_t s_context_ms;
static int s_x, s_y;
static bool s_visible;
static const lv_image_dsc_t *s_last_frame;
static int s_last_y;
static unsigned s_last_role = ~0u;
static int s_last_machine = -1;
static uint64_t s_role_entered_ms;



static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* Native RGB565+A8 arrays stay in memory-mapped Flash. No source frame buffer,
 * GIF decoder, scaling, rotation, recolor or intermediate layer is requested. */
static void render(void)
{
    if (!s_image || !s_visible) return;
    const spirit_player_t *player = &s_players[s_machine];
    uint64_t now = now_ms();
    unsigned action = s_theme >= 0 ? (unsigned)s_theme : (unsigned)player->action;
    uint64_t entered = s_theme >= 0 ? s_context_ms : player->entered_ms;
    uint64_t elapsed = now >= entered ? now - entered : 0;
    unsigned family = s_family;
    unsigned role = app_spirit_skin_for_machine(s_machine);
    if (role != s_last_role || s_machine != s_last_machine) {
        s_last_role = role; s_last_machine = s_machine; s_role_entered_ms = now;
    }
    if (role >= 4 && s_role_entered_ms > entered) {
        entered = s_role_entered_ms;
        elapsed = now >= entered ? now - entered : 0;
    }
    // New spirits play the selected model + generic pair once, then hold a
    // genuine source pose. Existing four-role timing remains unchanged.
    bool finite = role >= 4 || action == SPIRIT_COMPLETED || action == SPIRIT_FAILED;
    // Play family then generic once for finite states; repeat both for loops.
    unsigned pack = family == 2 ? 2 : ((elapsed / 4000) % 2 ? 2 : family);
    uint64_t end = family == 2 ? 4000 : 8000;
    bool complete = finite && (elapsed >= end || (action == SPIRIT_COMPLETED && s_restore_hold[s_machine]));
    if (complete) pack = 2;
    const spirit_device_clip_t *clip = spirit_external_clip(role, pack, action);
    if (!clip) clip = spirit_device_clip(role, pack, action);
    unsigned tick = complete ? 24 : (unsigned)((elapsed % 4000) / 160);
    const lv_image_dsc_t *frame = clip->frames[clip->timeline[tick]];
    if (frame != s_last_frame) {
        lv_image_set_src(s_image, frame);
        s_last_frame = frame;
    }
    int y = s_y;
    if (y != s_last_y) {
        lv_obj_set_pos(s_image, s_x, y);
        s_last_y = y;
    }
    /* The celebration is a one-shot. Keep its final pointer without waking a
     * timer until a state/page change asks for playback again. */
    if (complete && s_timer) lv_timer_pause(s_timer);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    render();
}

void app_spirit_view_log_budget(const char *reason)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "%s: LVGL free=%u max_used=%u biggest=%u; internal free=%u min=%u biggest=%u",
             reason, (unsigned)mon.free_size, (unsigned)mon.max_used,
             (unsigned)mon.free_biggest_size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void app_spirit_view_create(lv_obj_t *parent, int x, int y)
{
    app_spirit_view_destroy();
    s_x = x;
    s_y = y;
    s_image = lv_image_create(parent);
    if (!s_image) {
        ESP_LOGE(TAG, "Sprite object allocation failed");
        return;
    }
    const lv_image_dsc_t *first = app_spirit_skin_frame(app_spirit_skin_for_machine(s_machine), SPIRIT_IDLE, 0);
    lv_image_set_src(s_image, first);
    lv_obj_set_pos(s_image, x, y);
    lv_obj_set_clickable(s_image, false);
    s_last_frame = first;
    s_last_y = y;
    s_visible = false;
    s_timer = lv_timer_create(tick, 160, NULL);
    if (s_timer) lv_timer_pause(s_timer);
    else ESP_LOGE(TAG, "Sprite timer allocation failed; keeping a static frame");
}

void app_spirit_view_destroy(void)
{
    s_visible = false;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_image) { lv_obj_delete(s_image); s_image = NULL; }
    s_last_frame = NULL;
}

void app_spirit_view_update(int machine, const spirit_status_t *status, uint64_t time_ms)
{
    if (machine < 0 || machine >= BAMBU_MACHINE_MAX) return;
    s_machine = machine;
    spirit_player_t *player = &s_players[machine];
    if (!player->initialized && status && status->gcode_state && !strcmp(status->gcode_state, "FINISH")) {
        bool ack = false;
        if (app_spirit_finish_saved(machine, status->task_token, &ack) ||
            (!status->task_token && status->job_generation == 0)) {
            s_restore_hold[machine] = true;
            player->finish_seen = true; player->finish_acknowledged = ack;
            player->finish_entered_ms = time_ms > 8000 ? time_ms - 8000 : 0;
            player->job_generation = status->job_generation;
        }
    }
    spirit_action_t previous = player->action;
    uint32_t previous_generation = player->job_generation;
    uint64_t previous_entered = player->entered_ms;
    bool initialized = player->initialized;
    spirit_player_update(player, status, time_ms);
    if (player->action != SPIRIT_COMPLETED) s_restore_hold[machine] = false;
    if (s_timer && s_visible && (!initialized || previous != player->action ||
                                previous_generation != player->job_generation ||
                                previous_entered != player->entered_ms)) {
        lv_timer_resume(s_timer);
        lv_timer_reset(s_timer);
        ESP_LOGI(TAG, "machine=%d action=%s", machine, spirit_action_name(player->action));
        app_spirit_view_log_budget("action");
    }
    render();
}

void app_spirit_view_set_visible(bool visible)
{
    if (!s_image || visible == s_visible) return;
    s_visible = visible;
    if (visible) {
        if (s_timer) { lv_timer_resume(s_timer); lv_timer_reset(s_timer); }
        render();
    } else {
        if (s_timer) lv_timer_pause(s_timer);
    }
}

bool app_spirit_view_acknowledge(int machine, const spirit_status_t *status, uint64_t time_ms)
{
    if (machine < 0 || machine >= BAMBU_MACHINE_MAX) return false;
    if (!spirit_player_acknowledge(&s_players[machine], status, time_ms)) return false;
    s_machine = machine;
    if (s_timer && s_visible) { lv_timer_resume(s_timer); lv_timer_reset(s_timer); }
    render();
    return true;
}

bool app_spirit_view_finish_acknowledged(int machine)
{
    return machine >= 0 && machine < BAMBU_MACHINE_MAX && s_players[machine].finish_acknowledged;
}

void app_spirit_view_context(const char *model, int theme, uint64_t time_ms)
{
    unsigned family = spirit_model_family(model);
    if (family != s_family || theme != s_theme) {
        s_family = family; s_theme = theme; s_context_ms = time_ms;
        if (s_timer && s_visible) { lv_timer_resume(s_timer); lv_timer_reset(s_timer); }
    }
}

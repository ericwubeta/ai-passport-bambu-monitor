#include "app_spirit_skins.h"

#include <stddef.h>
#include <string.h>
#include <stdatomic.h>

#include "spirit_device_assets.h"
#include "spirit_external_assets.h"
#include "app_mqtt.h"
#include "app_notify.h"
#include "spirit_bindings.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define SKIN_NVS_NAMESPACE "print_spirit"
#define SKIN_NVS_KEY "skin"

static spirit_bindings_t s_bindings = {.version = 1};
static spirit_bindings_t s_pending;
static bool s_bindings_dirty;
static uint64_t s_boot_finishes[SPIRIT_BINDING_MAX];
static bool s_boot_acks[SPIRIT_BINDING_MAX];
static uint64_t s_last_save_attempt_ms;

static const char *TAG = "spirit_skins";
static _Atomic(uint32_t) s_init_state; // 0 not started, 1 loading, 2 complete.
static _Atomic(uint32_t) s_current = SPIRIT_SKIN_FISH;
static _Atomic(uint32_t) s_last_saved = UINT32_MAX;
static _Atomic(uint32_t) s_busy;
static _Atomic(uint32_t) s_save_status = SPIRIT_SKIN_SAVE_IDLE;

static bool valid_skin(spirit_skin_id_t id)
{
    return (unsigned)id < SPIRIT_SKIN_COUNT;
}

void app_spirit_skins_init(void)
{
    spirit_external_init();
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&s_init_state, &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return;
    }

    uint8_t stored = SPIRIT_SKIN_FISH;
    bool known_persisted = false;
    esp_err_t error = nvs_flash_init();
    if (error != ESP_OK) {
        // Do not copy the legacy config initializer's erase-on-error path:
        // Wi-Fi, account and printer settings belong to a different module.
        ESP_LOGW(TAG, "NVS initialization failed; using fish: %s",
                 esp_err_to_name(error));
    } else {
        nvs_handle_t handle;
        error = nvs_open(SKIN_NVS_NAMESPACE, NVS_READONLY, &handle);
        if (error == ESP_OK) {
            error = nvs_get_u8(handle, SKIN_NVS_KEY, &stored);
            size_t size = sizeof(s_bindings);
            esp_err_t be = nvs_get_blob(handle, "bindings_v1", &s_bindings, &size);
            if (be != ESP_OK || size != sizeof(s_bindings) || !spirit_bindings_valid(&s_bindings)) {
                memset(&s_bindings, 0, sizeof(s_bindings)); s_bindings.version = 1;
            }
            nvs_close(handle);
            if (error == ESP_OK && stored < SPIRIT_SKIN_COUNT) {
                known_persisted = true;
            } else {
                if (error != ESP_ERR_NVS_NOT_FOUND) {
                    ESP_LOGW(TAG, "Invalid/unreadable skin; using fish");
                }
                stored = SPIRIT_SKIN_FISH;
                // Missing/invalid values also resolve to fish at the next boot.
                known_persisted = error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND;
            }
        } else if (error == ESP_ERR_NVS_NOT_FOUND) {
            // No namespace yet: default fish requires no write.
            known_persisted = true;
        } else {
            ESP_LOGW(TAG, "Skin namespace unavailable; using fish: %s",
                     esp_err_to_name(error));
        }
    }

    for (int i = 0; i < SPIRIT_BINDING_MAX; i++) {
        s_boot_finishes[i] = s_bindings.entries[i].finish_token;
        s_boot_acks[i] = s_bindings.entries[i].finish_ack != 0;
    }
    atomic_store_explicit(&s_current, stored, memory_order_release);
    if (known_persisted) {
        atomic_store_explicit(&s_last_saved, stored, memory_order_release);
    }
    ESP_LOGI(TAG, "Loaded skin=%u", (unsigned)stored);
    atomic_store_explicit(&s_init_state, 2, memory_order_release);
}

spirit_skin_id_t app_spirit_skin_current(void)
{
    uint32_t id = atomic_load_explicit(&s_current, memory_order_acquire);
    return id < SPIRIT_SKIN_COUNT ? (spirit_skin_id_t)id : SPIRIT_SKIN_FISH;
}

const char *app_spirit_skin_name(spirit_skin_id_t id)
{
    static const char *const names[SPIRIT_SKIN_COUNT] = {
        "大肥鱼", "Claude Code", "Codex 白", "Codex 黑", "芙宁娜", "胡桃"
    };
    if (id == SPIRIT_SKIN_FURINA && !spirit_external_ready()) return "芙宁娜(资源未就绪)";
    if (id == SPIRIT_SKIN_HUTAO && !spirit_external_ready()) return "胡桃(资源未就绪)";
    return names[valid_skin(id) ? (unsigned)id : SPIRIT_SKIN_FISH];
}

bool app_spirit_skin_available(spirit_skin_id_t id)
{
    return valid_skin(id) && ((unsigned)id < 4 || spirit_external_ready());
}

const lv_image_dsc_t *app_spirit_skin_frame(spirit_skin_id_t id,
                                           spirit_action_t action, uint8_t frame)
{
    if ((unsigned)id >= 4) {
        const spirit_device_clip_t *clip = spirit_external_clip((unsigned)id, 2, (unsigned)action);
        if (clip) return clip->frames[frame < 4 && clip->frames[frame] ? frame : 0];
    }
    return spirit_device_frame((unsigned)id, 2, (unsigned)action, frame);
}

spirit_skin_id_t app_spirit_skin_for_machine(int machine)
{
    spirit_binding_t *entry = spirit_binding_find(&s_bindings, app_mqtt_sn(machine));
    return entry ? (spirit_skin_id_t)entry->skin : app_spirit_skin_current();
}

bool app_spirit_finish_saved(int machine, uint64_t token, bool *ack)
{
    spirit_binding_t *entry = spirit_binding_find(&s_bindings, app_mqtt_sn(machine));
    if (!entry || !token) return false;
    ptrdiff_t index = entry - s_bindings.entries;
    if (s_boot_finishes[index] != token) return false;
    if (ack) *ack = s_boot_acks[index];
    return true;
}

static void save_worker(void *argument)
{
    (void)argument;
    // The creating task publishes the new choice only after xTaskCreate
    // succeeds, then releases this notification gate. This avoids a worker
    // racing its caller even when the caller runs at a lower priority.
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    esp_err_t error = nvs_flash_init();
    if (error == ESP_OK) {
        nvs_handle_t handle;
        error = nvs_open(SKIN_NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (error == ESP_OK) {
            error = nvs_set_blob(handle, "bindings_v1", &s_pending, sizeof(s_pending));
            if (error == ESP_OK) error = nvs_commit(handle);
            nvs_close(handle);
        }
    }

    if (error == ESP_OK) {

        atomic_store_explicit(&s_save_status, SPIRIT_SKIN_SAVE_SAVED,
                              memory_order_release);
        ESP_LOGI(TAG, "Saved printer spirit bindings");
    } else {
        ESP_LOGW(TAG, "Skin save failed: %s", esp_err_to_name(error));
        atomic_store_explicit(&s_save_status, SPIRIT_SKIN_SAVE_FAILED,
                              memory_order_release);
    }
    // No status/current writes after making the next selection possible.
    atomic_store_explicit(&s_busy, 0, memory_order_release);
    vTaskDelete(NULL);
}

// All binding mutations run in the LVGL consumer. Worker owns only s_pending.
static bool queue_bindings(void)
{
    if (!s_bindings_dirty) return true;
    if (app_notify_busy()) {
        atomic_store(&s_save_status, SPIRIT_SKIN_SAVE_SAVING);
        return false;
    }
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, 1)) return false;
    s_pending = s_bindings;
    s_last_save_attempt_ms = (uint64_t)(esp_timer_get_time() / 1000);
    atomic_store(&s_save_status, SPIRIT_SKIN_SAVE_SAVING);
    TaskHandle_t worker = NULL;
    if (xTaskCreate(save_worker, "spirit_binding_save", 3072, NULL, 3, &worker) != pdPASS) {
        atomic_store(&s_save_status, SPIRIT_SKIN_SAVE_FAILED); atomic_store(&s_busy, 0); return false;
    }
    s_bindings_dirty = false;
    xTaskNotifyGive(worker); return true;
}

void app_spirit_bindings_poll(void)
{
    if (atomic_load(&s_save_status) == SPIRIT_SKIN_SAVE_FAILED) {
        s_bindings_dirty = true;
        uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
        if (now >= s_last_save_attempt_ms && now - s_last_save_attempt_ms < 30000) return;
    }
    for (int i = 0; i < app_mqtt_machine_count(); i++) {
        if (!spirit_binding_find(&s_bindings, app_mqtt_sn(i)) &&
            spirit_binding_ensure(&s_bindings, app_mqtt_sn(i), (uint8_t)app_spirit_skin_current())) s_bindings_dirty = true;
    }
    (void)queue_bindings();
}

bool app_spirit_skin_select_for_machine(int machine, spirit_skin_id_t id)
{
    if (!app_spirit_skin_available(id)) return false;
    spirit_binding_t *entry = spirit_binding_ensure(&s_bindings, app_mqtt_sn(machine), (uint8_t)app_spirit_skin_current());
    if (!entry) {
        atomic_store(&s_save_status, SPIRIT_SKIN_SAVE_FAILED);
        return false;
    }
    if (entry->skin != id) {
        app_notify_epoch(machine);
        entry->skin = (uint8_t)id; s_bindings_dirty = true;
    }
    return queue_bindings();
}

bool app_spirit_skin_select(spirit_skin_id_t id)
{
    return app_spirit_skin_select_for_machine(0, id);
}

void app_spirit_finish_remember(int machine, uint64_t token, bool ack)
{
    if (!token) return;
    spirit_binding_t *entry = spirit_binding_ensure(&s_bindings, app_mqtt_sn(machine), (uint8_t)app_spirit_skin_current());
    if (!entry) return;
    if (entry->finish_token == token && (!ack || entry->finish_ack)) return;
    entry->finish_token = token; entry->finish_ack = ack; s_bindings_dirty = true;
    (void)queue_bindings();
}

int app_spirit_skin_save_status(void)
{
    return (int)atomic_load_explicit(&s_save_status, memory_order_acquire);
}

bool app_spirit_bindings_busy(void)
{
    return atomic_load_explicit(&s_busy, memory_order_acquire) != 0;
}


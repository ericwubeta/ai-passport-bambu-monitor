#include "app_poweroff.h"
#include "app_screen_policy.h"
#include "app_camera.h"
#include "app_mqtt.h"
#include "app_notify.h"
#include "app_wifi.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "poweroff";
static portMUX_TYPE s_guard = portMUX_INITIALIZER_UNLOCKED;
static screen_poweroff_state_t s_policy;
static bool s_screen_off, s_wake_pending, s_requested, s_committed;
static uint32_t s_epoch;
typedef struct { uint32_t epoch, print_sequence; } poweroff_request_t;
static poweroff_request_t s_request;

bool app_poweroff_committed(void) {
    portENTER_CRITICAL(&s_guard);
    bool committed = s_committed;
    portEXIT_CRITICAL(&s_guard);
    return committed;
}

bool app_poweroff_activity(void) {
    portENTER_CRITICAL(&s_guard);
    bool allowed = !s_committed;
    s_epoch++;
    if (allowed) screen_poweroff_reset(&s_policy);
    portEXIT_CRITICAL(&s_guard);
    return allowed;
}

static void request_finished(void) {
    portENTER_CRITICAL(&s_guard);
    s_requested = false;
    screen_poweroff_reset(&s_policy);
    portEXIT_CRITICAL(&s_guard);
}

static void poweroff_task(void *arg) {
    const poweroff_request_t request = *(const poweroff_request_t *)arg;
    if (!bsp_lvgl_lock(500)) goto cancelled;
    // Check live inputs again after scheduling/lock waits. A pending print event
    // wins even when the corresponding idle task has already finished.
    bool safe = !bsp_battery_usb_connected() && !app_mqtt_has_active_job() &&
                app_mqtt_screen_wake_sequence() == request.print_sequence &&
                bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
    portENTER_CRITICAL(&s_guard);
    safe = safe && !s_committed && s_screen_off && !s_wake_pending && request.epoch == s_epoch;
    if (safe) s_committed = true;
    portEXIT_CRITICAL(&s_guard);
    if (!safe) { bsp_lvgl_unlock(); goto cancelled; }

    ESP_LOGI(TAG, "Battery idle screen-off timeout; preparing deep sleep");
    // Run teardown in this task, never in the shared esp_timer callback. After
    // commit, failures restart the application rather than leave dead inputs.
    esp_err_t err;
#if CONFIG_PM_ENABLE
    // On battery there is no USB guard. Keep automatic light sleep from
    // changing wake timers or peripheral clocks during terminal preparation.
    esp_pm_lock_handle_t shutdown_lock;
    err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "shutdown", &shutdown_lock);
    if (err != ESP_OK) goto restart;
    err = esp_pm_lock_acquire(shutdown_lock);
    if (err != ESP_OK) goto restart;
#endif
    app_camera_stop();
    app_wifi_pause_reconnect(60000);
    app_mqtt_stop();
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) goto restart;
    app_notify_stop();
    err = bsp_battery_sleep();
    if (err != ESP_OK) goto restart;
    err = bsp_i2c_prepare_deep_sleep();
    if (err != ESP_OK) goto restart;
    err = bsp_display_prepare_deep_sleep();
    if (err != ESP_OK) goto restart;
    int level;
    err = bsp_button_prepare_deep_sleep(&level);
    if (err != ESP_OK) goto restart;
    err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (err != ESP_OK) goto restart;
    err = esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) goto restart;

    // An insertion/print event during teardown, or a newly pressed key, restarts
    // into the normal application. No partially deinitialized screen is reused.
    portENTER_CRITICAL(&s_guard);
    bool activity = s_epoch != request.epoch;
    portEXIT_CRITICAL(&s_guard);
    if (activity || level != 1 || gpio_get_level(BSP_BTN_GPIO) != 1 || bsp_battery_usb_connected() ||
        app_mqtt_has_active_job() || app_mqtt_screen_wake_sequence() != request.print_sequence) {
        ESP_LOGI(TAG, "Activity arrived during shutdown; restarting");
        esp_restart();
        return;
    }
    ESP_LOGI(TAG, "Deep sleep; press any key to restart");
    esp_deep_sleep_start();
    return;

restart:
    ESP_LOGE(TAG, "Shutdown preparation failed: %s; restarting", esp_err_to_name(err));
    esp_restart();
    return;
cancelled:
    request_finished();
    vTaskDelete(NULL);
}

void app_poweroff_poll(bool screen_off, bool wake_pending) {
    bool usb = bsp_battery_usb_connected();
    bool active = app_mqtt_has_active_job();
    uint32_t print_sequence = app_mqtt_screen_wake_sequence();
    bool create = false;
    portENTER_CRITICAL(&s_guard);
    s_screen_off = screen_off;
    s_wake_pending = wake_pending;
    if (!s_committed) {
        bool was_counting = s_policy.counting;
        bool due = screen_poweroff_poll(&s_policy, screen_off, usb, active, wake_pending, esp_timer_get_time());
        if (was_counting && !s_policy.counting) s_epoch++;
        if (due && !s_requested) {
            s_request = (poweroff_request_t){.epoch=s_epoch, .print_sequence=print_sequence};
            s_requested = true;
            create = true;
        }
    }
    portEXIT_CRITICAL(&s_guard);
    if (create && xTaskCreate(poweroff_task, "poweroff", 5120, &s_request, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Cannot start shutdown task; keeping the device awake for events");
        request_finished();
    }
}

// Exercise actual MQTT activity detection and screen sleep/wake callbacks.
#define esp_timer_get_time unused_fixed_time
#include "camera_review_common.h"
#undef esp_timer_get_time
#include "app_mqtt.h"
#define CONFIG_PM_ENABLE 1
static int64_t now_us = 1000000, s_key_us = 1000000;
static int64_t esp_timer_get_time(void) { return now_us; }
static bool s_screen_off, s_swallow_key, s_swallow_released, usb_connected;
static bool printer_suspended, lvgl_suspended;
static int backlight = 100, s_awake_lock = 1, awake_balance = 1;
static int s_lock = 1, s_mcount = 1;
static bambu_state_t s_state[BAMBU_MACHINE_MAX];
#define xSemaphoreTake(...) 1
#define xSemaphoreGive(...) ((void)0)
static bool bsp_battery_usb_connected(void) { return usb_connected; }
static bool bsp_lvgl_lock(int timeout) { return true; }
static void bsp_lvgl_unlock(void) {}
static void app_printer_set_suspended(bool value) { printer_suspended = value; }
static void bsp_lvgl_set_suspended(bool value) { lvgl_suspended = value; }
static void bsp_display_backlight(int value) { backlight = value; }
static void esp_pm_lock_acquire(int lock) { awake_balance++; }
static void esp_pm_lock_release(int lock) { awake_balance--; }
typedef enum { BSP_BTN_PRESS, BSP_BTN_RELEASE, BSP_BTN_CLICK, BSP_BTN_DOUBLE } bsp_btn_ev_t;
/* @PRINTING@ */
/* @WAKE@ */
/* @TICK@ */
/* @KEY@ */
static void tick(int seconds) { now_us = seconds * 1000000LL; screen_off_tick(NULL); }
static void assert_dark(void) {
    assert(s_screen_off && backlight == 0 && printer_suspended && lvgl_suspended && awake_balance == 0);
}
static void assert_lit(void) {
    assert(!s_screen_off && backlight == 100 && !printer_suspended && !lvgl_suspended && awake_balance == 1);
}
static void reset(void) {
    now_us = s_key_us = 1000000;
    s_screen_off = s_swallow_key = s_swallow_released = false;
    printer_suspended = lvgl_suspended = false;
    backlight = 100; awake_balance = 1;
    s_lock = s_mcount = 1;
    memset(s_state, 0, sizeof(s_state));
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *scenario = argv[1];
    usb_connected = true;
    s_state[0].cloud = BAMBU_CLOUD_ONLINE;
    if (!strcmp(scenario, "inactive")) {
        const char *states[] = {"", "IDLE", "PAUSE", "FINISH", "FAILED", "PREPARE"};
        for (int power = 0; power <= 1; power++) {
            for (unsigned i = 0; i < sizeof(states)/sizeof(states[0]); i++) {
                reset(); usb_connected = power; s_state[0].cloud = BAMBU_CLOUD_ONLINE;
                strlcpy(s_state[0].gcode_state, states[i], sizeof(s_state[0].gcode_state));
                tick(30); assert_lit(); tick(31); assert_dark(); tick(90); assert_dark();
            }
        }
    } else if (!strcmp(scenario, "finish") || !strcmp(scenario, "unplug")) {
        strlcpy(s_state[0].gcode_state, "RUNNING", sizeof(s_state[0].gcode_state));
        tick(90); assert_lit();
        if (!strcmp(scenario, "finish")) strlcpy(s_state[0].gcode_state, "FINISH", sizeof(s_state[0].gcode_state));
        else usb_connected = false;
        tick(91); assert_lit(); tick(119); assert_lit(); tick(120); assert_dark();
    } else if (!strcmp(scenario, "battery")) {
        usb_connected = false;
        strlcpy(s_state[0].gcode_state, "RUNNING", sizeof(s_state[0].gcode_state));
        tick(31); assert_dark();
    } else if (!strcmp(scenario, "multi")) {
        s_mcount = 2; s_state[1].cloud = BAMBU_CLOUD_ONLINE;
        strlcpy(s_state[1].gcode_state, "RUNNING", sizeof(s_state[1].gcode_state));
        tick(90); assert_lit();
        s_mcount = 1; tick(120); assert_dark(); // Unconfigured slots cannot exempt sleep.
    } else if (!strcmp(scenario, "offline")) {
        strlcpy(s_state[0].gcode_state, "RUNNING", sizeof(s_state[0].gcode_state));
        s_state[0].cloud = BAMBU_CLOUD_CONNECTING;
        tick(31); assert_dark();
        s_lock = 0; tick(32); assert_dark(); // Startup/no MQTT is not activity.
    } else if (!strcmp(scenario, "wake")) {
        usb_connected = false; tick(31); assert_dark();
        usb_connected = true; tick(32); assert_dark(); // Plugging in while idle stays dark.
        strlcpy(s_state[0].gcode_state, "RUNNING", sizeof(s_state[0].gcode_state));
        tick(33); assert_lit(); tick(90); assert_lit();
    } else if (!strcmp(scenario, "key")) {
        tick(31); assert_dark();
        assert(screen_off_on_key(BSP_BTN_PRESS)); assert_lit();
        assert(screen_off_on_key(BSP_BTN_RELEASE));
        assert(screen_off_on_key(BSP_BTN_CLICK));
        tick(60); assert_lit(); tick(61); assert_dark();
    } else assert(false);
    return 0;
}

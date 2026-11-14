// Exercise actual MQTT activity detection and screen sleep/wake callbacks.
#define esp_timer_get_time unused_fixed_time
#include "camera_review_common.h"
#undef esp_timer_get_time
#include "app_mqtt.h"
#include "app_notify.h"
#include "app_screen_policy.h"
#define CONFIG_PM_ENABLE 1
#define BSP_BTN_MV_RELEASED_MIN 1900
static int64_t now_us = 1000000;
static screen_wake_state_t s_wake_policy = {.usb_connected=true};
static screen_idle_state_t s_idle_policy = {.idle_since_us=1000000};
static int64_t esp_timer_get_time(void) { return now_us; }
static bool s_screen_off, s_swallow_key, s_swallow_released, usb_connected;
static bool printer_suspended, lvgl_suspended;
static int backlight = 100, s_awake_lock = 1, awake_balance = 1;
static int s_lock = 1, s_mcount = 1;
static bambu_state_t s_state[BAMBU_MACHINE_MAX];
static bool s_ever_data[BAMBU_MACHINE_MAX], lvgl_available = true;
static atomic_uint s_screen_wake_sequence;
static int sounds;
#define xSemaphoreTake(...) 1
#define xSemaphoreGive(...) ((void)0)
static bool bsp_battery_usb_connected(void) { return usb_connected; }
bool app_poweroff_committed(void) { return false; }
bool app_poweroff_activity(void) { return true; }
void app_poweroff_poll(bool off, bool pending) {}
static bool bsp_lvgl_lock(int timeout) { return lvgl_available; }
static void bsp_lvgl_unlock(void) {}
static void app_printer_set_suspended(bool value) { printer_suspended = value; }
static void bsp_lvgl_set_suspended(bool value) { lvgl_suspended = value; }
static void bsp_display_backlight(int value) { backlight = value; }
static void esp_pm_lock_acquire(int lock) { awake_balance++; }
static void esp_pm_lock_release(int lock) { awake_balance--; }
typedef enum { BSP_BTN_PRESS, BSP_BTN_RELEASE, BSP_BTN_CLICK, BSP_BTN_DOUBLE } bsp_btn_ev_t;
void app_notify_trigger(notify_kind_t kind) { sounds++; }
/* @MQTT@ */
/* @PRINTING@ */
/* @SEQUENCE@ */
/* @WAKE@ */
/* @TICK@ */
/* @KEY@ */
/* @BOOT_KEY@ */
static void tick(int seconds) { now_us = seconds * 1000000LL; screen_off_tick(NULL); }
static void assert_dark(void) {
    assert(s_screen_off && backlight == 0 && printer_suspended && lvgl_suspended && awake_balance == 0);
}
static void assert_lit(void) {
    assert(!s_screen_off && backlight == 100 && !printer_suspended && !lvgl_suspended && awake_balance == 1);
}
static void reset(void) {
    now_us = 1000000;
    screen_idle_init(&s_idle_policy, now_us);
    s_wake_policy = (screen_wake_state_t){.usb_connected=true};
    atomic_store(&s_screen_wake_sequence, 0);
    usb_connected = lvgl_available = true;
    sounds = 0;
    s_screen_off = s_swallow_key = s_swallow_released = false;
    printer_suspended = lvgl_suspended = false;
    backlight = 100; awake_balance = 1;
    s_lock = s_mcount = 1;
    memset(s_state, 0, sizeof(s_state));
    s_state[0].cloud = BAMBU_CLOUD_ONLINE;
}
static void report(int machine, const char *state) {
    char json[100];
    snprintf(json, sizeof(json), "{\"print\":{\"gcode_state\":\"%s\"}}", state);
    parse_report(machine, json, (int)strlen(json));
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *scenario = argv[1];
    reset();
    s_state[0].cloud = BAMBU_CLOUD_ONLINE;
    if (!strcmp(scenario, "boot-key")) {
        boot_key_gesture(true,3300); assert(!screen_off_on_key(BSP_BTN_PRESS));
        boot_key_gesture(true,3300); assert(screen_off_on_key(BSP_BTN_CLICK));
        assert(!screen_off_on_key(BSP_BTN_PRESS));
        boot_key_gesture(true,300); assert(screen_off_on_key(BSP_BTN_PRESS));
        assert(screen_off_on_key(BSP_BTN_RELEASE)); assert(screen_off_on_key(BSP_BTN_CLICK));
        assert(!screen_off_on_key(BSP_BTN_PRESS));
        boot_key_gesture(false,3300); assert(!screen_off_on_key(BSP_BTN_PRESS));
        boot_key_gesture(false,0); assert(screen_off_on_key(BSP_BTN_PRESS));
    } else if (!strcmp(scenario, "inactive")) {
        const char *states[] = {"", "IDLE", "PAUSE", "FINISH", "FAILED", "PREPARE"};
        for (int power = 0; power <= 1; power++) {
            for (unsigned i = 0; i < sizeof(states)/sizeof(states[0]); i++) {
                reset(); usb_connected = power; s_state[0].cloud = BAMBU_CLOUD_ONLINE;
                s_wake_policy.usb_connected = power;
                strlcpy(s_state[0].gcode_state, states[i], sizeof(s_state[0].gcode_state));
                tick(30); assert_lit(); tick(31); assert_dark(); tick(90); assert_dark();
            }
        }
    } else if (!strcmp(scenario, "finish") || !strcmp(scenario, "unplug")) {
        strlcpy(s_state[0].gcode_state, "RUNNING", sizeof(s_state[0].gcode_state));
        tick(90); assert_lit();
        if (!strcmp(scenario, "finish")) strlcpy(s_state[0].gcode_state, "FINISH", sizeof(s_state[0].gcode_state));
        else usb_connected = false;
        tick(91); assert_lit(); tick(120); assert_lit(); tick(121); assert_dark();
    } else if (!strcmp(scenario, "battery")) {
        usb_connected = false;
        strlcpy(s_state[0].gcode_state, "RUNNING", sizeof(s_state[0].gcode_state));
        tick(31); assert_dark();
    } else if (!strcmp(scenario, "multi")) {
        s_mcount = 2; s_state[1].cloud = BAMBU_CLOUD_ONLINE;
        strlcpy(s_state[1].gcode_state, "RUNNING", sizeof(s_state[1].gcode_state));
        tick(90); assert_lit();
        s_mcount = 1; tick(91); tick(120); assert_lit(); tick(121); assert_dark();
    } else if (!strcmp(scenario, "offline")) {
        strlcpy(s_state[0].gcode_state, "RUNNING", sizeof(s_state[0].gcode_state));
        s_state[0].cloud = BAMBU_CLOUD_CONNECTING;
        tick(31); assert_dark();
        s_lock = 0; tick(32); assert_dark(); // Startup/no MQTT is not activity.
    } else if (!strcmp(scenario, "wake")) {
        usb_connected = false; tick(31); assert_dark();
        usb_connected = true; tick(32); assert_lit();
        tick(61); assert_lit(); tick(62); assert_dark(); tick(90); assert_dark();
        usb_connected = false; tick(91); assert_dark();
        usb_connected = true; tick(92); assert_lit(); tick(122); assert_dark();
    } else if (!strcmp(scenario, "events")) {
        for (int power=0; power<=1; power++) {
            reset(); usb_connected = s_wake_policy.usb_connected = power;
            report(0, "IDLE"); tick(31); assert_dark();
            report(0, "RUNNING"); tick(32); assert_lit();
            assert(app_mqtt_screen_wake_sequence() == 1);
            report(0, "RUNNING"); tick(90);
            if (power) assert_lit(); else assert_dark();
            report(0, "FINISH"); tick(91); assert_lit();
            assert(app_mqtt_screen_wake_sequence() == 2 && sounds == 1);
            report(0, "FINISH"); tick(110); assert_lit();
            tick(120); assert_lit(); tick(121); assert_dark(); tick(180); assert_dark();
            assert(app_mqtt_screen_wake_sequence() == 2 && sounds == 1);
        }
    } else if (!strcmp(scenario, "initial")) {
        const char *states[] = {"RUNNING", "FINISH"};
        for (int power=0; power<=1; power++) for (int i=0; i<2; i++) {
            reset(); usb_connected = s_wake_policy.usb_connected = power;
            tick(31); assert_dark(); report(0, states[i]);
            tick(40); assert_dark(); report(0, states[i]); tick(41); assert_dark();
            assert(app_mqtt_screen_wake_sequence() == 0 && sounds == 0);
        }
        reset(); usb_connected = s_wake_policy.usb_connected = false;
        report(0, "IDLE"); tick(31); assert_dark();
        report(0, "FINISH"); tick(32); assert_dark();
        report(0, "FAILED"); tick(33); assert_dark();
        assert(app_mqtt_screen_wake_sequence() == 0);
    } else if (!strcmp(scenario, "multi-event")) {
        usb_connected = s_wake_policy.usb_connected = false; s_mcount = 2;
        report(0, "IDLE"); report(1, "PAUSE"); tick(31); assert_dark();
        report(1, "RUNNING"); tick(32); assert_lit(); tick(62); assert_dark();
        report(0, "FINISH"); tick(63); assert_dark(); // Other machine's baseline cannot complete this job.
        report(1, "PAUSE"); tick(64); assert_dark();
        report(1, "FINISH"); tick(65); assert_lit();
        assert(app_mqtt_screen_wake_sequence() == 2);
    } else if (!strcmp(scenario, "priority")) {
        usb_connected = s_wake_policy.usb_connected = false;
        report(0, "IDLE"); report(0, "RUNNING"); tick(31); assert_lit();
        tick(60); assert_lit(); tick(61); assert_dark();
    } else if (!strcmp(scenario, "retry")) {
        usb_connected = s_wake_policy.usb_connected = false;
        report(0, "IDLE"); tick(31); assert_dark();
        lvgl_available = false; report(0, "RUNNING"); tick(32); assert_dark();
        assert(s_wake_policy.pending); tick(40); assert_dark();
        lvgl_available = true; tick(41); assert_lit(); assert(!s_wake_policy.pending);
        tick(70); assert_lit(); tick(71); assert_dark();
    } else if (!strcmp(scenario, "wrap")) {
        usb_connected = s_wake_policy.usb_connected = false;
        report(0, "IDLE"); tick(31); assert_dark();
        atomic_store(&s_screen_wake_sequence, UINT32_MAX);
        s_wake_policy.print_sequence = UINT32_MAX;
        report(0, "RUNNING"); assert(app_mqtt_screen_wake_sequence() == 0);
        report(0, "FINISH"); tick(32); assert_lit();
        tick(62); assert_dark();
    } else if (!strcmp(scenario, "telemetry")) {
        usb_connected = s_wake_policy.usb_connected = false;
        report(0, "IDLE"); tick(31); assert_dark();
        report(0, "RUNNING"); tick(32); assert_lit(); tick(62); assert_dark();
        const char *delta = "{\"print\":{\"mc_percent\":50,\"gcode_state\":null}}";
        parse_report(0, delta, (int)strlen(delta)); tick(63); assert_dark();
        s_state[0].cloud = BAMBU_CLOUD_CONNECTING; tick(64); assert_dark();
        s_state[0].cloud = BAMBU_CLOUD_ONLINE; report(0, "RUNNING"); tick(65); assert_dark();
        assert(app_mqtt_screen_wake_sequence() == 1);
        report(0, "FINISH"); tick(66); assert_lit();
        assert(app_mqtt_screen_wake_sequence() == 2);
    } else if (!strcmp(scenario, "lit-reset")) {
        usb_connected = s_wake_policy.usb_connected = false;
        report(0, "IDLE"); tick(20); assert_lit();
        report(0, "RUNNING"); tick(21); assert_lit();
        report(0, "RUNNING"); tick(40); assert_lit();
        tick(50); assert_lit(); tick(51); assert_dark();
    } else if (!strcmp(scenario, "key-retry")) {
        tick(31); assert_dark(); lvgl_available = false;
        assert(screen_off_on_key(BSP_BTN_PRESS)); assert_dark();
        lvgl_available = true; tick(32); assert_lit();
        assert(screen_off_on_key(BSP_BTN_RELEASE));
        assert(screen_off_on_key(BSP_BTN_CLICK));
        assert(!screen_off_on_key(BSP_BTN_PRESS));
    } else if (!strcmp(scenario, "key")) {
        tick(31); assert_dark();
        assert(screen_off_on_key(BSP_BTN_PRESS)); assert_lit();
        assert(screen_off_on_key(BSP_BTN_RELEASE));
        assert(screen_off_on_key(BSP_BTN_CLICK));
        tick(60); assert_lit(); tick(61); assert_dark();
    } else assert(false);
    return 0;
}

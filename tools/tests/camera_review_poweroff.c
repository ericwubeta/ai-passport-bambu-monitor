#define esp_timer_get_time unused_fixed_time
#include "camera_review_common.h"
#undef esp_timer_get_time
#include "app_screen_policy.h"
#include "app_poweroff.h"
#include "app_mqtt.h"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define CONFIG_PM_ENABLE 1
#define ESP_PM_NO_LIGHT_SLEEP 0
#define BSP_BTN_GPIO 0
#define BSP_BTN_MV_RELEASED_MIN 1900
#define pdPASS 1
#define ESP_GPIO_WAKEUP_GPIO_LOW 0
#define ESP_SLEEP_WAKEUP_ALL 0
#define xSemaphoreTake(...) 1
#define xSemaphoreGive(...) ((void)0)
typedef int esp_err_t;
typedef int esp_pm_lock_handle_t;
enum { ESP_OK, ESP_ERR_WIFI_NOT_INIT, ESP_ERR_WIFI_NOT_STARTED, ESP_FAIL };
static screen_poweroff_state_t s_policy;
static bool s_screen_off, s_wake_pending, s_requested, s_committed;
static uint32_t s_epoch;
typedef struct { uint32_t epoch, print_sequence; } poweroff_request_t;
static poweroff_request_t s_request;
static int64_t now_us;
static bool usb, locked, stopped, wifi_stopped, audio_stopped, battery_asleep, i2c_asleep, display_asleep, wake_cleared, pm_locked;
static int requests, deep_sleeps, restarts, cancelled, s_lock=1, s_mcount=1;
static bambu_state_t s_state[BAMBU_MACHINE_MAX];
static uint32_t print_sequence;
static void (*deferred)(void *);
static void *deferred_arg;
static const char *scenario;
static int64_t esp_timer_get_time(void) { return now_us; }
static bool bsp_battery_usb_connected(void) { return usb; }
uint32_t app_mqtt_screen_wake_sequence(void) { return print_sequence; }
/* @ACTIVE_JOB@ */
static int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int priority, void *handle) {
    requests++;
    if (!strcmp(scenario,"oom")) return 0;
    deferred=fn; deferred_arg=arg; return 1;
}
static void vTaskDelete(void *task) { cancelled++; }
static bool bsp_lvgl_lock(int timeout) { locked=strcmp(scenario,"lock-busy") != 0; return locked; }
static void bsp_lvgl_unlock(void) { locked=false; }
static int bsp_button_read_mv(void) {
    return !strcmp(scenario,"adc-error") ? -1 : !strcmp(scenario,"held") ? 300 : 3300;
}
static esp_err_t esp_pm_lock_create(int kind, int arg, const char *name, int *out) {
    assert(kind==ESP_PM_NO_LIGHT_SLEEP); *out=1; return !strcmp(scenario,"pm-error") ? ESP_FAIL : ESP_OK;
}
static esp_err_t esp_pm_lock_acquire(int lock) { pm_locked=true; return ESP_OK; }
static void app_camera_stop(void) { assert(locked); }
static void app_wifi_pause_reconnect(unsigned ms) { assert(locked && ms > 0); }
void app_mqtt_stop(void) { assert(locked && pm_locked); stopped=true; }
static esp_err_t esp_wifi_stop(void) {
    assert(stopped); wifi_stopped=true;
    if (!strcmp(scenario,"late-usb")) usb=true;
    if (!strcmp(scenario,"late-print")) { strcpy(s_state[0].gcode_state,"RUNNING"); print_sequence++; }
    return !strcmp(scenario,"wifi-error") ? ESP_FAIL : !strcmp(scenario,"wifi-not-started") ? ESP_ERR_WIFI_NOT_STARTED : ESP_OK;
}
static void app_notify_stop(void) { assert(wifi_stopped); audio_stopped=true; }
static esp_err_t bsp_battery_sleep(void) { assert(audio_stopped); battery_asleep=true; return !strcmp(scenario,"battery-error") ? ESP_FAIL : ESP_OK; }
static esp_err_t bsp_i2c_prepare_deep_sleep(void) { assert(battery_asleep); i2c_asleep=true; return !strcmp(scenario,"i2c-error") ? ESP_FAIL : ESP_OK; }
static esp_err_t bsp_display_prepare_deep_sleep(void) { assert(i2c_asleep); display_asleep=true; return !strcmp(scenario,"display-error") ? ESP_FAIL : ESP_OK; }
static esp_err_t bsp_button_prepare_deep_sleep(int *level) {
    assert(display_asleep); *level=1;
    if (!strcmp(scenario,"late-key")) assert(!app_poweroff_activity());
    return !strcmp(scenario,"button-error") ? ESP_FAIL : ESP_OK;
}
static esp_err_t esp_sleep_disable_wakeup_source(int source) {
    assert(source==ESP_SLEEP_WAKEUP_ALL); wake_cleared=true; return ESP_OK;
}
static esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t mask, int mode) {
    assert(wake_cleared && mask==1 && mode==ESP_GPIO_WAKEUP_GPIO_LOW); return !strcmp(scenario,"wake-error") ? ESP_FAIL : ESP_OK;
}
static int gpio_get_level(int gpio) { return !strcmp(scenario,"gpio-low") ? 0 : 1; }
static void esp_restart(void) { restarts++; }
static void esp_deep_sleep_start(void) { assert(stopped && wifi_stopped && audio_stopped && battery_asleep && i2c_asleep && display_asleep); deep_sleeps++; }
/* @COMMITTED@ */
/* @ACTIVITY@ */
/* @FINISHED@ */
/* @TASK@ */
/* @POLL@ */
static void poll_at(int64_t time, bool off, bool pending) { now_us=time; app_poweroff_poll(off,pending); }
static void request(void) {
    poll_at(1000000,false,false); poll_at(30000000,true,false);
    poll_at(629999999,true,false); assert(requests==0);
    poll_at(630000000,true,false); assert(requests==1);
}
int main(int argc, char **argv) {
    assert(argc==2); scenario=argv[1];
    strcpy(s_state[0].gcode_state,"FINISH");
    if (!strcmp(scenario,"guards")) {
        const char *jobs[]={"RUNNING","PAUSE","PREPARE"};
        s_mcount=2;
        for (unsigned i=0;i<3;i++) {
            strcpy(s_state[1].gcode_state,jobs[i]);
            for (int cloud=0;cloud<3;cloud++) {
                s_state[1].cloud=cloud; assert(app_mqtt_has_active_job());
                poll_at(now_us+1000000000,true,false); assert(requests==0 && !s_policy.counting);
            }
        }
        strcpy(s_state[1].gcode_state,"IDLE"); usb=true;
        poll_at(now_us+1000000000,true,false); assert(requests==0);
        usb=false; poll_at(now_us+1,false,false); assert(!s_policy.counting);
        return 0;
    }
    if (!strcmp(scenario,"reset")) {
        poll_at(0,true,false); poll_at(599999999,true,false); assert(requests==0);
        usb=true; poll_at(600000000,true,false); assert(requests==0);
        usb=false; poll_at(700000000,true,false);
        poll_at(1299999999,true,false); assert(requests==0);
        assert(app_poweroff_activity()); poll_at(1300000000,true,false);
        poll_at(1899999999,true,false); assert(requests==0);
        poll_at(1900000000,true,true); assert(requests==0 && !s_policy.counting);
        poll_at(2000000000,true,false); poll_at(1,true,false);
        assert(!screen_poweroff_poll(&s_policy,true,false,false,false,600000000));
        return 0;
    }
    request();
    if (!strcmp(scenario,"oom")) { assert(!s_requested && !s_policy.counting && !stopped); return 0; }
    poll_at(631000000,true,false); assert(requests==1);
    if (!strcmp(scenario,"queued-key")) assert(app_poweroff_activity());
    if (!strcmp(scenario,"queued-usb")) usb=true;
    if (!strcmp(scenario,"queued-print")) strcpy(s_state[0].gcode_state,"RUNNING");
    if (!strcmp(scenario,"queued-event")) print_sequence++;
    if (!strcmp(scenario,"queued-wake")) poll_at(now_us,true,true);
    deferred(deferred_arg);
    bool abort_before=!strncmp(scenario,"queued-",7) || !strcmp(scenario,"lock-busy") || !strcmp(scenario,"adc-error") || !strcmp(scenario,"held");
    bool reboot=!strncmp(scenario,"late-",5) || strstr(scenario,"-error") || !strcmp(scenario,"gpio-low");
    if (abort_before) assert(!stopped && !s_committed && cancelled==1 && !s_requested && !locked);
    else if (reboot) assert(restarts==1 && deep_sleeps==0 && s_committed);
    else assert(deep_sleeps==1 && restarts==0 && s_committed);
    return 0;
}

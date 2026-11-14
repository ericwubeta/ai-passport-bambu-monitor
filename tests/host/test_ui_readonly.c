// Actual Core LVGL page construction and queued button navigation, dummy telemetry.
#include "lvgl.h"
#include "app_printer.h"
#include "app_camera.h"
#include "app_config.h"
#include "app_mqtt.h"
#include "app_notify.h"
#include "app_wifi.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t clock_ms = 1000;
static int machine_count = 1;
static bambu_state_t states[4];
static unsigned refresh_requests;
static unsigned captured_frames;
static bool camera_active;
static unsigned camera_refresh_requests;
static bool restore_view;
static printer_view_config_t saved_view;
static uint8_t display_buffer[240 * 40 * 2];
static uint8_t screen[240 * 320 * 3];

void host_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
int app_mqtt_machine_count(void) { return machine_count; }
void app_mqtt_snapshot(int index, bambu_state_t *out)
{ assert(index >= 0 && index < machine_count); *out = states[index]; }
const char *app_mqtt_sn(int index)
{
    static const char *serials[] = {"CORE-HOST-1", "CORE-HOST-2", "CORE-HOST-3", "CORE-HOST-4"};
    assert(index >= 0 && index < machine_count); return serials[index];
}
const char *app_mqtt_name(int index)
{
    static const char *names[] = {"Fixture1", "Fixture2", "Fixture3", "Fixture4"};
    assert(index >= 0 && index < machine_count); return names[index];
}
int app_mqtt_uptime_sec(void) { return 3600; }
void app_mqtt_request_now(int index)
{ assert(index >= 0 && index < machine_count); refresh_requests++; }
void app_notify_init(void) {}
void app_notify_trigger(notify_kind_t kind)
{ assert(kind >= NOTIFY_FINISH && kind <= NOTIFY_PAUSE); }
bool app_wifi_connected(void) { return true; }
bool app_wifi_time_ready(void) { return false; }
int app_wifi_rssi(void) { return -55; }
const char *app_wifi_current_ssid(void) { return "CoreHost"; }
void app_wifi_ip_str(char *out, int size) { snprintf(out, (size_t)size, "192.0.2.1"); }
int bsp_battery_soc(void) { return 75; }
int bsp_battery_mv(void) { return 4000; }
bool bsp_battery_usb_connected(void) { return false; }
bool config_printer_view_load(printer_view_config_t *view)
{
    if (!restore_view) return false;
    *view = saved_view;
    return true;
}
bool config_printer_view_save(const printer_view_config_t *view)
{ saved_view = *view; return true; }
void app_camera_start(int machine, int x, int y, int width, int height)
{
    assert(machine >= 0 && machine < machine_count);
    assert(x == 0 && y == 92 && width == 240 && height == 135);
    camera_active = true;
}
void app_camera_stop(void) { camera_active = false; }
void app_camera_refresh(void) { assert(camera_active); camera_refresh_requests++; }
void app_camera_snapshot(camera_state_t *out)
{ *out = (camera_state_t){.status = CAMERA_UNCONFIGURED}; }

static uint32_t ticks(void) { return clock_ms; }

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *input = (const uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; y++) for (int x = area->x1; x <= area->x2; x++) {
        uint16_t value = *input++;
        assert(x >= 0 && x < 240 && y >= 0 && y < 320);
        uint8_t *out = &screen[(y * 240 + x) * 3];
        out[0] = (uint8_t)(((value >> 11) & 31) * 255 / 31);
        out[1] = (uint8_t)(((value >> 5) & 63) * 255 / 63);
        out[2] = (uint8_t)((value & 31) * 255 / 31);
    }
    lv_display_flush_ready(display);
}

static void check_no_controls(lv_obj_t *object)
{
    if (lv_obj_check_type(object, &lv_label_class)) {
        const char *text = lv_label_get_text(object);
        assert(!strstr(text, "bambumonitor"));
        assert(!strstr(text, "打印控制") && !strstr(text, "暂停打印") && !strstr(text, "继续打印"));
        assert(!strstr(text, "停止打印") && !strstr(text, "腔灯") && !strstr(text, "OK 执行"));
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) check_no_controls(lv_obj_get_child(object, i));
}

static void advance(unsigned duration)
{
    for (unsigned i = 0; i < duration; i += 20) { clock_ms += 20; lv_timer_handler(); }
    check_no_controls(lv_screen_active());
}

static void press(bsp_btn_t button, bsp_btn_ev_t event)
{ app_printer_key(button, event); advance(80); }

static void append_labels(lv_obj_t *object, char *out, size_t capacity)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(object, &lv_label_class)) {
        size_t used = strlen(out);
        const char *text = lv_label_get_text(object);
        assert(used + strlen(text) + 2 < capacity);
        strcat(out, text); strcat(out, "\n");
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) append_labels(lv_obj_get_child(object, i), out, capacity);
}

static void signature(char *out, size_t capacity)
{ out[0] = 0; append_labels(lv_screen_active(), out, capacity); }

static void capture(const char *name)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    char path[96]; snprintf(path, sizeof(path), "%s.ppm", name);
    FILE *out = fopen(path, "wb"); assert(out);
    fprintf(out, "P6\n240 320\n255\n"); fwrite(screen, 1, sizeof(screen), out); fclose(out);
    captured_frames++;
}

static void set_fixture(int ams_count, bool reported)
{
    memset(states, 0, sizeof(states));
    for (int index = 0; index < machine_count; index++) {
        bambu_state_t *state = &states[index];
        state->cloud = BAMBU_CLOUD_ONLINE;
        if (reported) strcpy(state->gcode_state, "RUNNING");
        state->pct = 37 + index;
        state->remain_min = 21;
        state->nozzle_t = 210; state->nozzle_target = 220;
        state->bed_t = 60; state->bed_target = 60;
        state->layer = 10; state->total_layer = 100;
        strcpy(state->job, "Core host fixture");
        state->ams_count = ams_count;
        for (int a = 0; a < ams_count; a++) {
            state->ams[a].present = true;
            state->ams[a].humidity = 2;
            for (int t = 0; t < 4; t++) {
                strcpy(state->ams[a].tray[t].tray_type, "PLA");
                state->ams[a].tray[t].is_used = true;
                state->ams[a].tray[t].tray_pct = 75;
                state->ams[a].tray[t].tray_remain = 750;
            }
        }
    }
}

static void verify_cycle(int page_count, bool screenshots)
{
    char pages[8][4096], current[4096];
    for (int i = 0; i < page_count; i++) {
        signature(pages[i], sizeof(pages[i]));
        assert(camera_active == (i == 1));
        for (int previous = 0; previous < i; previous++) assert(strcmp(pages[i], pages[previous]));
        if (screenshots) {
            char name[48]; snprintf(name, sizeof(name), "core-page-%d", i); capture(name);
        }
        unsigned before = refresh_requests;
        unsigned camera_before = camera_refresh_requests;
        press(BSP_BTN_OK, BSP_BTN_CLICK);
        if (i == 0) {
            assert(refresh_requests == before);
            press(BSP_BTN_OK, BSP_BTN_CLICK); // Restore remaining/ETA mode.
        } else if (i == 1) {
            assert(refresh_requests == before && camera_refresh_requests == camera_before + 1);
        } else {
            assert(refresh_requests == before + 1);
        }
        press(BSP_BTN_DOWN, BSP_BTN_CLICK);
    }
    signature(current, sizeof(current)); assert(!strcmp(current, pages[0]));
    press(BSP_BTN_UP, BSP_BTN_CLICK);
    signature(current, sizeof(current)); assert(!strcmp(current, pages[page_count - 1]));
    assert(strstr(current, "PrintPet Core v1.5.2-host"));
    unsigned before = refresh_requests;
    press(BSP_BTN_OK, BSP_BTN_CLICK); assert(refresh_requests == before + 1);
    press(BSP_BTN_DOWN, BSP_BTN_CLICK);
}

int main(void)
{
    lv_init(); lv_tick_set_cb(ticks);
    lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    machine_count = 1;
    set_fixture(0, false); app_printer_enter(); advance(100);
    verify_cycle(4, false); app_printer_exit();

    // A deployed camera slot survives restart. The retired control slot falls
    // back to progress on the same printer.
    restore_view = true;
    saved_view = (printer_view_config_t){.version = 1, .slot = 8, .serial = "CORE-HOST-1"};
    app_printer_enter(); advance(100); assert(camera_active);
    app_printer_exit(); assert(!camera_active);
    saved_view.slot = 7;
    app_printer_enter(); advance(100);
    verify_cycle(4, false); app_printer_exit();
    restore_view = false;
    for (int ams = 0; ams <= 4; ams++) {
        set_fixture(ams, true); app_printer_enter(); advance(100);
        int pages = 4 + (ams ? ams : 1);
        verify_cycle(pages, ams == 1);
        app_printer_exit();
        printf("PASS: %d AMS, %d monitoring pages, bidirectional wrap and OK refresh\n", ams, pages);
    }
    for (int count = 2; count <= 4; count++) {
        machine_count = count; set_fixture(1, true);
        for (int round = 0; round < 5; round++) {
            app_printer_enter(); advance(100); assert(app_printer_in_overview());
            for (int i = 1; i < count; i++) press(BSP_BTN_DOWN, BSP_BTN_CLICK);
            press(BSP_BTN_OK, BSP_BTN_CLICK); assert(!app_printer_in_overview());
            verify_cycle(5, false);
            press(BSP_BTN_OK, BSP_BTN_LONG); assert(app_printer_in_overview());
            app_printer_exit();
        }
    }
    printf("PASS: 2-4 printer overview/detail reentry; %u refreshes; %u rendered captures; no control objects\n",
           refresh_requests, captured_frames);
    return 0;
}

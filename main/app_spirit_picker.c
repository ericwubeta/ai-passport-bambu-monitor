#include "app_spirit_picker.h"

#include "app_spirit_skins.h"
#include "app_mqtt.h"
#include "font_zh.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdatomic.h>

static int s_machine;
static bool s_choose_machine;
static lv_obj_t *s_heading;
static lv_obj_t *s_screen, *s_image, *s_name, *s_status, *s_help;
static lv_obj_t *s_dots[SPIRIT_SKIN_COUNT];
static lv_timer_t *s_timer;
static spirit_skin_id_t s_focus;
static spirit_player_t s_preview;
static const lv_image_dsc_t *s_last_frame;
static int s_last_y;
static int s_last_focus, s_last_save, s_last_current;
static atomic_int s_pending_input;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static lv_obj_t *text(const lv_font_t *font, uint32_t color, const char *value, int y)
{
    lv_obj_t *label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, value);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static void refresh(void)
{
    const char *name = app_mqtt_name(s_machine);
    lv_label_set_text(s_heading, app_mqtt_machine_count() > 1 ? (name ? name : app_mqtt_sn(s_machine)) : "精灵");
    lv_obj_align(s_heading, LV_ALIGN_TOP_MID, 0, 16);
    spirit_sample_t sample = spirit_player_sample(&s_preview, now_ms());
    const lv_image_dsc_t *frame = app_spirit_skin_frame(s_focus, SPIRIT_IDLE, sample.frame_index);
    if (frame != s_last_frame) {
        lv_image_set_src(s_image, frame);
        s_last_frame = frame;
    }
    int y = 64 + sample.y_offset;
    if (s_last_y != y) { lv_obj_set_pos(s_image, 64, y); s_last_y = y; }
    bool focus_changed = s_last_focus != (int)s_focus;
    if (focus_changed) {
        lv_label_set_text(s_name, app_spirit_skin_name(s_focus));
        lv_obj_align(s_name, LV_ALIGN_TOP_MID, 0, 212);
        for (int i = 0; i < SPIRIT_SKIN_COUNT; i++) {
            lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i == (int)s_focus ? 0x34D97B : 0x34404D), 0);
        }
        s_last_focus = (int)s_focus;
    }
    int saving = app_spirit_skin_save_status();
    int current = (int)app_spirit_skin_for_machine(s_machine);
    {
        const char *message = "";
        if (!app_spirit_skin_available(s_focus)) message = "资源未就绪";
        else if (saving == SPIRIT_SKIN_SAVE_SAVING) message = "保存中...";
        else if (saving == SPIRIT_SKIN_SAVE_FAILED) message = "保存失败，请重试";
        else if ((int)s_focus == current) message = "当前精灵";
        lv_label_set_text(s_status, message);
        lv_obj_set_style_text_color(s_status, lv_color_hex(saving == SPIRIT_SKIN_SAVE_FAILED ? 0xE5484D : 0x34D97B), 0);
        lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 258);
        s_last_save = saving;
        s_last_current = current;
    }
    lv_label_set_text(s_help, s_choose_machine ? "上下选机器 · OK 选择" : "上下选择 · OK 使用");
    lv_obj_align(s_help, LV_ALIGN_TOP_MID, 0, 282);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_screen) return;
    int input = atomic_exchange_explicit(&s_pending_input, 0, memory_order_acq_rel);
    if (input) {
        bsp_btn_t key = (bsp_btn_t)(input - 1);
        if (s_choose_machine) {
            int count = app_mqtt_machine_count();
            if (key == BSP_BTN_OK) s_choose_machine = false;
            else s_machine = (s_machine + (key == BSP_BTN_UP ? count - 1 : 1)) % count;
            s_focus = app_spirit_skin_for_machine(s_machine);
            s_last_focus = -1;
        } else if (key == BSP_BTN_UP || key == BSP_BTN_DOWN) {
            int delta = key == BSP_BTN_UP ? SPIRIT_SKIN_COUNT - 1 : 1;
            s_focus = (spirit_skin_id_t)(((int)s_focus + delta) % SPIRIT_SKIN_COUNT);
            s_preview.entered_ms = now_ms();
        } else if (key == BSP_BTN_OK) {
            app_spirit_skin_select_for_machine(s_machine, s_focus);
            if (app_mqtt_machine_count() > 1) s_choose_machine = true;
        }
    }
    app_spirit_bindings_poll();
    refresh();
}

void app_spirit_picker_enter(void)
{
    if (s_screen) return;
    s_machine = 0;
    s_choose_machine = app_mqtt_machine_count() > 1;
    s_focus = app_spirit_skin_for_machine(s_machine);
    s_preview = (spirit_player_t){.action = SPIRIT_IDLE, .initialized = true, .entered_ms = now_ms()};
    atomic_store_explicit(&s_pending_input, 0, memory_order_release);
    s_screen = lv_obj_create(NULL);
    lv_obj_set_scrollable(s_screen, false);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0B0F16), 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    s_heading = text(&font_zh14, 0xE6EDF3, "精灵", 16);
    lv_obj_set_width(s_heading, 220);
    lv_obj_set_style_text_align(s_heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_heading, LV_LABEL_LONG_DOT);
    s_image = lv_image_create(s_screen);
    lv_image_set_src(s_image, app_spirit_skin_frame(s_focus, SPIRIT_IDLE, 0));
    lv_obj_set_pos(s_image, 64, 64);
    lv_obj_set_clickable(s_image, false);
    s_last_frame = NULL;
    s_last_y = 64;
    s_last_focus = s_last_save = s_last_current = -1;
    s_name = text(&font_zh20, 0xE6EDF3, "", 212);
    for (int i = 0; i < SPIRIT_SKIN_COUNT; i++) {
        s_dots[i] = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_dots[i]);
            lv_obj_set_pos(s_dots[i], (240 - (SPIRIT_SKIN_COUNT - 1) * 14 - 6) / 2 + i * 14, 244);
        lv_obj_set_size(s_dots[i], 6, 4);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
    }
    s_status = text(&font_zh14, 0x34D97B, "", 258);
    s_help = text(&font_zh14, 0x6B7A8C, "", 282);
    text(&font_zh14, 0x6B7A8C, "长按 OK 返回", 302);
    s_timer = lv_timer_create(tick, 40, NULL);
    refresh();
    lv_screen_load(s_screen);
}

void app_spirit_picker_exit(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    atomic_store_explicit(&s_pending_input, 0, memory_order_release);
    if (s_screen) { lv_obj_delete(s_screen); s_screen = NULL; }
    s_last_frame = NULL;
}

void app_spirit_picker_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    if (!s_screen || event != BSP_BTN_CLICK) return;
    atomic_store_explicit(&s_pending_input, (int)button + 1, memory_order_release);
}

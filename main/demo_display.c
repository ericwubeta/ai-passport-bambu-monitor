// main/demo_display.c —— 背光亮度调节。
// UP/DOWN 预览亮度,OK 确认保存到 NVS(下次开机沿用),退出不再强制恢复 100%。
#include "demo.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

static lv_obj_t *s_scr;
static lv_obj_t *s_info;
static lv_obj_t *s_mascot;
static int s_bl_idx;
static bool s_dirty;

static const uint8_t BL_LEVELS[] = { 100, 60, 30, 10 };
#define BL_COUNT (sizeof(BL_LEVELS) / sizeof(BL_LEVELS[0]))

#define NVS_NS   "display"
#define NVS_KEY  "bl_idx"

static int load_bl(void)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, NVS_KEY, &v) != ESP_OK || v < 0 || v >= (int32_t)BL_COUNT) v = 0;
        nvs_close(h);
    }
    return (int)v;
}

static void save_bl(int idx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY, idx);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void refresh(void)
{
    lv_label_set_text_fmt(s_info, "背光亮度\n\n%d%%\n\nUP/DOWN: 调节\nOK: 确认保存",
                          BL_LEVELS[s_bl_idx]);
}

void demo_display_enter(void)
{
    s_bl_idx = load_bl();
    s_dirty = false;
    bsp_display_backlight(BL_LEVELS[s_bl_idx]);

    s_scr = ui_pixel_screen_create("BRIGHTNESS");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 18, 70, 204, 160, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_info);
    s_mascot = ui_pixel_mascot_create(s_scr, 101, 238);
    refresh();
    lv_screen_load(s_scr);
}

void demo_display_exit(void)
{
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; s_info = s_mascot = NULL; }
}

void demo_display_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) {
        save_bl(s_bl_idx);                 // 确认保存,下次开机沿用
        s_dirty = false;
        lv_label_set_text_fmt(s_info, "已保存 %d%%\n\nUP/DOWN: 调节\nOK: 重新保存",
                              BL_LEVELS[s_bl_idx]);
        ui_pixel_mascot_jump(s_mascot);
    } else {
        s_bl_idx = (btn == BSP_BTN_UP) ? (s_bl_idx + BL_COUNT - 1) % BL_COUNT
                                       : (s_bl_idx + 1) % BL_COUNT;
        bsp_display_backlight(BL_LEVELS[s_bl_idx]);
        s_dirty = true;
        refresh();
        ui_pixel_mascot_jump(s_mascot);
    }
}

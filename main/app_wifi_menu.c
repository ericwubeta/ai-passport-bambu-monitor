// main/app_wifi_menu.c —— 设备端手动切网:列出已保存的 WiFi,上下选择,OK 切换。
// 长按 OK 返回菜单由 main.c 统一处理(这里不拦截)。
#include "app_wifi_menu.h"

#include <stdio.h>
#include <string.h>

#include "app_wifi.h"
#include "font_zh.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "wifi_menu";

#define C_BG     0x0B0F16
#define C_PANEL  0x141B25
#define C_LINE   0x1E2A38
#define C_GREEN  0x00C853
#define C_GREEN_L 0x34D97B
#define C_TEXT   0xE6EDF3
#define C_DIM    0x6B7A8C
#define C_RED    0xE5484D

#define ROW_MAX  4
#define ROW_Y0   68
#define ROW_STEP 48

static lv_obj_t *s_scr;
static lv_obj_t *s_rows[ROW_MAX];
static lv_obj_t *s_marks[ROW_MAX];      // 右侧"当前"标记
static lv_obj_t *s_status;
static lv_timer_t *s_poll;
static int  s_count;
static int  s_sel;
static int  s_poll_n;
static char s_target[33];

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    return l;
}

// 当前连接标记 / 选中高亮随状态刷新
static void refresh(void)
{
    const char *cur = app_wifi_current_ssid();
    for (int i = 0; i < s_count; i++) {
        bool sel = (i == s_sel);
        bool is_cur = cur && app_wifi_saved_ssid(i) &&
                      strcmp(cur, app_wifi_saved_ssid(i)) == 0;
        lv_obj_set_style_border_width(s_rows[i], sel ? 2 : 1, 0);
        lv_obj_set_style_border_color(s_rows[i],
            lv_color_hex(sel ? C_GREEN : C_LINE), 0);
        if (is_cur) lv_obj_remove_flag(s_marks[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(s_marks[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// 轮询切换结果:目标 SSID 与驱动实际关联一致即成功;15 秒未连上判失败
static void poll_cb(lv_timer_t *t)
{
    if (!s_scr) { lv_timer_delete(t); s_poll = NULL; return; }
    s_poll_n++;
    const char *cur = app_wifi_current_ssid();
    if (cur && strcmp(cur, s_target) == 0) {
        lv_label_set_text(s_status, "已切换");
        lv_obj_set_style_text_color(s_status, lv_color_hex(C_GREEN_L), 0);
        refresh();
        lv_timer_delete(t);
        s_poll = NULL;
        return;
    }
    if (s_poll_n >= 30) {
        lv_label_set_text(s_status, "切换失败:密码或信号问题");
        lv_obj_set_style_text_color(s_status, lv_color_hex(C_RED), 0);
        lv_timer_delete(t);
        s_poll = NULL;
    }
}

static void do_switch(void)
{
    const char *target = app_wifi_saved_ssid(s_sel);
    if (!target) return;
    if (!app_wifi_switch_to(s_sel)) return;
    strlcpy(s_target, target, sizeof(s_target));
    lv_label_set_text(s_status, "切换中…");
    lv_obj_set_style_text_color(s_status, lv_color_hex(C_DIM), 0);
    s_poll_n = 0;
    if (s_poll) lv_timer_reset(s_poll);
    else        s_poll = lv_timer_create(poll_cb, 500, NULL);
    ESP_LOGI(TAG, "手动切换到 [%d] %s", s_sel, target);
}

void app_wifi_menu_enter(void)
{
    if (s_scr) return;
    s_count = app_wifi_saved_count();
    if (s_count > ROW_MAX) s_count = ROW_MAX;
    if (s_sel >= s_count) s_sel = 0;

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    lv_obj_t *title = mk_label(s_scr, &font_zh20, C_GREEN_L);
    lv_label_set_text(title, "切换 WiFi");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *sep = lv_obj_create(s_scr);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(sep, 0, 36);
    lv_obj_set_size(sep, 240, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);

    lv_obj_t *hint = mk_label(s_scr, &font_zh14, C_DIM);
    lv_label_set_text(hint, "上下选择 · OK 切换");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 42);

    if (s_count == 0) {
        lv_obj_t *empty = mk_label(s_scr, &font_zh14, C_DIM);
        lv_label_set_text(empty, "没有已保存的 WiFi\n请先进配网页添加");
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 110);
    }

    for (int i = 0; i < s_count; i++) {
        lv_obj_t *row = lv_obj_create(s_scr);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(row, 10, ROW_Y0 + i * ROW_STEP);
        lv_obj_set_size(row, 220, 44);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_PANEL), 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(C_LINE), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        s_rows[i] = row;

        lv_obj_t *nm = mk_label(row, &font_zh14, C_TEXT);
        lv_label_set_text(nm, app_wifi_saved_ssid(i));
        lv_obj_set_width(nm, 150);                    // 长名字末尾省略
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 10, 0);

        s_marks[i] = mk_label(row, &font_zh14, C_GREEN_L);
        lv_label_set_text(s_marks[i], "当前");
        lv_obj_align(s_marks[i], LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_add_flag(s_marks[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_status = mk_label(s_scr, &font_zh14, C_DIM);
    lv_label_set_text(s_status, "");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 226);

    lv_obj_t *foot = mk_label(s_scr, &font_zh14, 0x445262);
    lv_label_set_text(foot, "长按 OK 返回菜单");
    lv_obj_align(foot, LV_ALIGN_TOP_MID, 0, 252);

    refresh();
    lv_screen_load(s_scr);
    ESP_LOGI(TAG, "切网页打开: 已存 %d 组", s_count);
}

void app_wifi_menu_exit(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    for (int i = 0; i < ROW_MAX; i++) { s_rows[i] = NULL; s_marks[i] = NULL; }
    s_status = NULL;
}

void app_wifi_menu_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_scr || ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP && s_count > 0) {
        s_sel = (s_sel + s_count - 1) % s_count;
        refresh();
    } else if (btn == BSP_BTN_DOWN && s_count > 0) {
        s_sel = (s_sel + 1) % s_count;
        refresh();
    } else if (btn == BSP_BTN_OK) {
        do_switch();
    }
}

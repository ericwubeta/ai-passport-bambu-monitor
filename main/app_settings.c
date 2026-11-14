// main/app_settings.c —— 菜单里的音量/亮度调节。
// 两个项目一行一个，OK 在两项之间切换，上下键直接调节当前项；长按 OK 返回菜单。
// 值保存在 print_spirit 命名空间，重启后继续生效。
#include "app_settings.h"

#include "app_notify.h"
#include "app_spirit_skins.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "font_zh.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdbool.h>

#define NVS_NAMESPACE   "print_spirit"
#define KEY_VOLUME      "volume"
#define KEY_BRIGHT      "bright"
#define STEP            10
#define VOLUME_DEFAULT  70
#define BRIGHT_DEFAULT  100
#define BRIGHT_MIN      10      // 0% 会像熄屏，亮度最低留 10%

#define C_BG      0x0B0F16
#define C_LINE    0x1E2A38
#define C_TEXT    0xE6EDF3
#define C_DIM     0x6B7A8C
#define C_GREEN   0x00C853
#define C_ORANGE  0xFF6B35

static unsigned s_volume = VOLUME_DEFAULT;
static unsigned s_bright = BRIGHT_DEFAULT;
static bool s_dirty;
static int s_sel;                       // 0 = 音量, 1 = 亮度

static lv_obj_t *s_screen;
static lv_obj_t *s_name[2], *s_value[2];
static lv_obj_t *s_bar[2][10];
static lv_obj_t *s_help, *s_back;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                       int x, int y, int w, lv_text_align_t align)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_align(o, align, 0);
    return o;
}

static void apply_volume(void) { bsp_audio_set_volume((uint8_t)s_volume); }
static void apply_bright(void) { bsp_display_backlight((uint8_t)s_bright); }

void app_settings_init(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t value = 0;
    if (nvs_get_u8(handle, KEY_VOLUME, &value) == ESP_OK && value <= 100) s_volume = value;
    if (nvs_get_u8(handle, KEY_BRIGHT, &value) == ESP_OK &&
        value >= BRIGHT_MIN && value <= 100) s_bright = value;
    nvs_close(handle);
}

unsigned app_settings_volume(void) { return s_volume; }
unsigned app_settings_brightness(void) { return s_bright; }

static void save(void)
{
    if (!s_dirty) return;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, KEY_VOLUME, (uint8_t)s_volume);
    nvs_set_u8(handle, KEY_BRIGHT, (uint8_t)s_bright);
    nvs_commit(handle);
    nvs_close(handle);
    s_dirty = false;
}

static void refresh(void)
{
    for (int row = 0; row < 2; row++) {
        unsigned value = row ? s_bright : s_volume;
        bool active = row == s_sel;
        lv_label_set_text_fmt(s_value[row], "%u%%", value);
        lv_obj_set_style_text_color(s_name[row], lv_color_hex(active ? C_GREEN : C_DIM), 0);
        lv_obj_set_style_text_color(s_value[row], lv_color_hex(active ? C_GREEN : C_TEXT), 0);
        int lit = (int)(value / STEP);
        for (int i = 0; i < 10; i++)
            lv_obj_set_style_bg_color(s_bar[row][i],
                lv_color_hex(i < lit ? (row ? C_ORANGE : C_GREEN) : C_LINE), 0);
    }
}

void app_settings_enter(void)
{
    if (s_screen) return;
    s_sel = 0;
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    lv_obj_t *title = label(s_screen, &font_zh20, C_TEXT, 20, 16, 200, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(title, "设置");
    // 版本号放在这里，设备页标题只保留机型名（用户要求不要跟在 X2D 后面）。
    lv_obj_t *version = label(s_screen, &font_zh14, C_DIM, 120, 20, 100, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(version, "v" FW_VERSION);

    for (int row = 0; row < 2; row++) {
        int y = 84 + row * 68;
        s_name[row] = label(s_screen, &font_zh14, C_DIM, 20, y, 90, LV_TEXT_ALIGN_LEFT);
        lv_label_set_text(s_name[row], row ? "亮度" : "音量");
        s_value[row] = label(s_screen, &font_zh14, C_TEXT, 130, y, 90, LV_TEXT_ALIGN_RIGHT);
        for (int i = 0; i < 10; i++) {
            lv_obj_t *block = lv_obj_create(s_screen);
            lv_obj_remove_style_all(block);
            lv_obj_set_pos(block, 20 + i * 20, y + 22);
            lv_obj_set_size(block, 16, 8);
            lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(block, lv_color_hex(C_LINE), 0);
            s_bar[row][i] = block;
        }
    }

    lv_obj_t *credit = label(s_screen, &font_zh14, C_DIM, 10, 208, 220, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(credit, "原神 / 米哈游\n芙宁娜·钱琛 / 胡桃·陶典\nAI克隆合成 · 非商用");

    s_help = label(s_screen, &font_zh14, C_DIM, 20, 282, 200, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_help, "OK 切换 · 上下调节");
    s_back = label(s_screen, &font_zh14, C_DIM, 20, 302, 200, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_back, "长按 OK 返回");

    refresh();
    lv_screen_load(s_screen);
}

void app_settings_exit(void)
{
    save();
    if (s_screen) { lv_obj_delete(s_screen); s_screen = NULL; }
}

void app_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_screen || ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_OK) {
        s_sel ^= 1;
        refresh();
        return;
    }
    if (btn != BSP_BTN_UP && btn != BSP_BTN_DOWN) return;

    int delta = btn == BSP_BTN_UP ? STEP : -STEP;
    if (s_sel == 0) {
        int value = (int)s_volume + delta;
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        if (value != (int)s_volume) {
            s_volume = (unsigned)value;
            s_dirty = true;
            apply_volume();
            // Audible preview of the new level; skip while a clip is already
            // playing so repeated presses cannot queue a backlog.
            if (!app_notify_busy())
                app_notify_page_voice(0, (unsigned)app_spirit_skin_for_machine(0), 0, 0);
        }
    } else {
        int value = (int)s_bright + delta;
        if (value < BRIGHT_MIN) value = BRIGHT_MIN;
        if (value > 100) value = 100;
        if (value != (int)s_bright) {
            s_bright = (unsigned)value;
            s_dirty = true;
            apply_bright();
        }
    }
    refresh();
}

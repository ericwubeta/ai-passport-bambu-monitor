// main/main.c —— AI 通行证固件入口:开机动画 → 打印机监控主界面;
// 长按确定回到演示菜单(原 BSP 参考示例保留)。
//
// 按键语义(全局统一):
//   上/下 短按   菜单中=移动选中项;打印机页=切换页面
//   确定  短按   菜单中=进入选中项;打印机页=立即刷新
//   确定  长按   任意页面=返回菜单
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "ui_pixel.h"
#include "app_boot.h"
#include "app_printer.h"
#include "app_wifi.h"
#include "app_mqtt.h"
#include "font_zh.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "main";

// 打印机页作为第 0 项进入主流程;下面 4 项是原 BSP 演示。
static const demo_entry_t DEMOS[] = {
    { "打印机", app_printer_enter, app_printer_exit, app_printer_key },
    { "显示",   demo_display_enter, demo_display_exit, demo_display_key },
    { "按键",   demo_button_enter,  demo_button_exit,  demo_button_key  },
    { "音频",   demo_audio_enter,   demo_audio_exit,   demo_audio_key   },
    { "电量",   demo_battery_enter, demo_battery_exit, demo_battery_key },
    { "酒桌游戏", demo_party_enter, demo_party_exit, demo_party_key },
};
#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))

// 各外设初始化结果:失败的项在菜单里标 [FAIL] 且不允许进入。
static bool s_ok[DEMO_COUNT];

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_cards[DEMO_COUNT];
static lv_obj_t *s_rows[DEMO_COUNT];
static lv_obj_t *s_mascot;
static int  s_sel;                 // 当前选中项
static int  s_active = -1;         // 当前所在页面;-1 = 在菜单
static bool s_in_printer = false;  // 主流程开机后直接进打印机页(不是经菜单进入)

static void menu_refresh(void) {
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        lv_label_set_text_fmt(s_rows[i], "%s%s",
                              DEMOS[i].name,
                              s_ok[i] ? "" : "  [FAIL]");
        ui_pixel_set_selected(s_cards[i], (int)i == s_sel, s_ok[i]);
        lv_obj_set_style_text_color(s_rows[i],
            s_ok[i] ? lv_color_hex(UI_INK) : lv_color_hex(0x7A2020), 0);
    }
}

static void menu_build(void) {
    s_menu_scr = ui_pixel_screen_create("AI 通行证");

    // 5 张卡片:2 列 3 行紧凑排布,给吉祥物留出脚下草地
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        int x = 11 + (int)(i % 2) * 112;
        int y = 52 + (int)(i / 2) * 64;
        s_cards[i] = ui_pixel_panel_create(s_menu_scr, x, y, 102, 54, UI_PAPER);
        s_rows[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_rows[i], &font_zh14, 0);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_rows[i]);
    }

    s_mascot = ui_pixel_mascot_create(s_menu_scr, 101, 246);

    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void enter_menu(void) {
    s_active = -1;
    s_in_printer = false;
    menu_build();
}

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

    if (s_active >= 0) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {     // 统一返回
            DEMOS[s_active].exit();
            enter_menu();
        } else {
            DEMOS[s_active].key(btn, ev);
        }
    } else if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP)   { s_sel = (s_sel + DEMO_COUNT - 1) % DEMO_COUNT; menu_refresh(); }
        if (btn == BSP_BTN_DOWN) { s_sel = (s_sel + 1) % DEMO_COUNT;              menu_refresh(); }
        if (btn == BSP_BTN_OK && s_ok[s_sel]) {
            s_active = s_sel;
            if (s_sel == 0) s_in_printer = true;
            ui_pixel_mascot_jump(s_mascot);
            lv_obj_delete(s_menu_scr);
            s_menu_scr = NULL;
            s_mascot = NULL;
            DEMOS[s_active].enter();
        } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            ui_pixel_mascot_jump(s_mascot);
        }
    }
    bsp_lvgl_unlock();
}

// 开机动画结束回调(此时已持 LVGL 锁):起网络,进打印机主界面
static void boot_done(void) {
    app_wifi_start();
    app_mqtt_start();
    s_active = 0;
    s_in_printer = true;
    app_printer_enter();
}

void app_main(void) {
    ESP_LOGI(TAG, "AI Passport 打印机监控固件启动");

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,固件无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    s_ok[0] = true;                                   // Printer 页始终可用
    s_ok[1] = true;                                   // Display 已确认可用
    s_ok[2] = (bsp_button_init(on_key, NULL) == ESP_OK);
    s_ok[3] = (bsp_audio_init() == ESP_OK);
    s_ok[4] = (bsp_battery_init() == ESP_OK);
    s_ok[5] = s_ok[2];                       // 酒桌游戏依赖按键,复用按键初始化结果

    // 开机动画(拓竹 logo)→ 直接进打印机监控页
    if (bsp_lvgl_lock(1000)) { app_boot_show(boot_done); bsp_lvgl_unlock(); }

    ESP_LOGI(TAG, "就绪:Button=%d Audio=%d Battery=%d",
             s_ok[2], s_ok[3], s_ok[4]);
}

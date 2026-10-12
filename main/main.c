// main/main.c —— AI Passport 独立打印机固件入口:开机动画 → 直达拓竹状态屏。
// 裁剪自多合一固件:仅保留打印机监控,长按确定回单页菜单。
//
// 按键语义(与主固件一致):
//   上/下 短按   打印机页=切换页面(状态/温度/AMS/设备/控制)
//   确定  短按   打印机页=立即刷新
//   确定  长按   返回菜单(单页,再按确定回到打印机页)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "ui_pixel.h"
#include "app_boot.h"
#include "app_provision.h"
#include "app_printer.h"
#include "app_wifi.h"
#include "app_mqtt.h"
#include "font_zh.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "main";

static const demo_entry_t DEMOS[] = {
    { "打印机", app_printer_enter,   app_printer_exit,   app_printer_key },
    { "配网",   app_provision_enter, app_provision_exit, app_provision_key },
};
#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))

static bool s_ok[DEMO_COUNT];

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_cards[DEMO_COUNT];
static lv_obj_t *s_rows[DEMO_COUNT];
static lv_obj_t *s_mascot;
static int  s_sel;                 // 当前选中项
static int  s_active = -1;         // 当前所在页面;-1 = 在菜单

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

    for (size_t i = 0; i < DEMO_COUNT; i++) {
        int cy = 140 - (int)(DEMO_COUNT - 1) * 35 + (int)i * 70;   // 多项时竖排居中
        s_cards[i] = ui_pixel_panel_create(s_menu_scr, 60, cy, 100, 46, UI_PAPER);
        s_rows[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_rows[i], &font_zh14, 0);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_rows[i]);
    }

    s_mascot = ui_pixel_mascot_create(s_menu_scr, 101, 286);

    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void enter_menu(void) {
    s_active = -1;
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
        if (btn == BSP_BTN_OK && s_ok[s_sel]) {
            s_active = s_sel;
            ui_pixel_mascot_jump(s_mascot);
            lv_obj_delete(s_menu_scr);
            s_menu_scr = NULL;
            s_mascot = NULL;
            DEMOS[s_active].enter();
        }
    }
    bsp_lvgl_unlock();
}

// 开机动画结束回调(此时已持 LVGL 锁):未配网→配网模式;已配置→联网直达打印机屏
static void boot_done(void) {
    if (app_provision_needed()) {
        ESP_LOGW(TAG, "设备未配网,进入配网模式(热点 + 网页)");
        app_provision_start();
        s_active = 1;
        DEMOS[1].enter();
        return;
    }
    app_wifi_start();
    app_mqtt_start();
    s_active = 0;
    DEMOS[0].enter();
}

void app_main(void) {
    ESP_LOGI(TAG, "AI Passport 打印机固件启动");

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,固件无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    s_ok[0] = (bsp_button_init(on_key, NULL) == ESP_OK);   // 页面导航依赖按键
    bsp_audio_init();                                       // 提示音(完成/失败/HMS)
    bsp_battery_init();                                     // 状态栏电量

    // 开机动画(拓竹 logo)→ 直接进打印机监控页
    if (bsp_lvgl_lock(1000)) { app_boot_show(boot_done); bsp_lvgl_unlock(); }

    ESP_LOGI(TAG, "就绪:Button=%d", s_ok[0]);
}

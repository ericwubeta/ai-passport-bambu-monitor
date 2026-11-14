// main/main.c —— PrintPet Core 固件入口:开机动画 → 直达打印状态屏。
// 裁剪自多合一固件:仅保留打印机监控,长按确定回单页菜单。
//
// 按键语义(与主固件一致):
//   上/下 短按   打印机页=切换只读页面(进度/相机/温度/AMS/设备)
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
#include "app_identity.h"
#include "app_provision.h"
#include "app_wifi_menu.h"
#include "app_printer.h"
#include "app_wifi.h"
#include "app_mqtt.h"
#include "app_camera.h"
#include "app_screen_policy.h"
#include "app_poweroff.h"
#include "font_zh.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "main";

// 空闲熄屏:30s 无按键 → 背光 0 并暂停 LVGL;任意键点亮,并吞掉唤醒它的那一次按键。
static screen_wake_state_t s_wake_policy;
static screen_idle_state_t s_idle_policy;
static bool    s_screen_off;
static bool    s_swallow_key;
static bool    s_swallow_released;
static esp_timer_handle_t s_scr_timer;

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_usb_lock;
static esp_timer_handle_t   s_usb_timer;
// 亮屏期间禁止 light sleep:LVGL 每 5ms 醒一次刷 SPI,睡眠/唤醒会打断刷新时序,
// 画面上表现为明显闪烁。只有熄屏(画面已冻结)之后才放锁,让它真正睡下去。
static esp_pm_lock_handle_t s_awake_lock;

// USB 守卫:IDF 自带的 USJ 连接监控只要 3ms 收不到 SOF 就放锁,而芯片复位/重枚举
// 的窗口有几十毫秒,正好够它睡下去;C3 的 USB-Serial-JTAG 在 light sleep 中不工作,
// 睡下去主机就再也枚举不到,只能断电重来(本版就在这上面栽过一次)。所以自己持一把
// NO_LIGHT_SLEEP 锁:启动先无条件持 5s 盖住重枚举窗口,之后每 5s 用
// usb_serial_jtag_is_connected() 复查,连着就继续持,真断了(用电池)才放。
static void usb_guard_cb(void *arg)
{
    (void)arg;
    if (app_poweroff_committed()) return;
    if (usb_serial_jtag_is_connected()) return;      // 还连着 USB,继续禁止 light sleep
    if (s_usb_lock) esp_pm_lock_release(s_usb_lock); // 已断线:放锁,自动 light sleep 生效
    if (s_usb_timer) esp_timer_stop(s_usb_timer);    // 断线后没必要再复查
    ESP_LOGI(TAG, "USB 已断开,允许 light sleep");
}

// 自动 light sleep + DFS(160↔80MHz)。
static void pm_init(void)
{
#if CONFIG_PM_ENABLE
    const esp_pm_config_t cfg = {
        .max_freq_mhz       = 160,
        .min_freq_mhz       = 80,
        .light_sleep_enable = true,
    };
    esp_err_t e = esp_pm_configure(&cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure 失败: %s", esp_err_to_name(e));
        return;
    }
    ESP_LOGI(TAG, "PM 就绪: DFS 80~160MHz, 自动 light sleep 开");

    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_guard", &s_usb_lock);
    if (s_usb_lock) esp_pm_lock_acquire(s_usb_lock);
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "awake", &s_awake_lock);
    if (s_awake_lock) esp_pm_lock_acquire(s_awake_lock);
    const esp_timer_create_args_t a = { .name = "usb_guard", .callback = usb_guard_cb };
    if (esp_timer_create(&a, &s_usb_timer) == ESP_OK)
        esp_timer_start_periodic(s_usb_timer, 5ULL * 1000 * 1000);
#endif
}
#endif

static bool screen_wake(void)
{
    if (!app_poweroff_activity()) return false;
    if (!s_screen_off) return true;
    if (!bsp_lvgl_lock(200)) return false;
#if CONFIG_PM_ENABLE
    if (s_awake_lock) esp_pm_lock_acquire(s_awake_lock);
#endif
    // Resume rendering before lighting the panel, as with a key wake.
    app_printer_set_suspended(false);
    bsp_lvgl_set_suspended(false);
    bsp_display_backlight(100);
    s_screen_off = false;
    bsp_lvgl_unlock();
    return true;
}

static void screen_off_tick(void *arg)
{
    (void)arg;
    if (app_poweroff_committed()) return;
    bool usb_connected = bsp_battery_usb_connected();
    if (usb_connected != s_wake_policy.usb_connected) {
        ESP_LOGI(TAG, "USB host power=%d, screen_off=%d", usb_connected, s_screen_off);
    }
    bool sleep_due = screen_idle_poll(&s_idle_policy, usb_connected,
                                      app_mqtt_any_printing(), esp_timer_get_time());
    bool wake_pending = screen_wake_poll(&s_wake_policy, usb_connected, app_mqtt_screen_wake_sequence());
    app_poweroff_poll(s_screen_off, wake_pending);
    if (wake_pending) {
        // Wake wins over a simultaneous timeout. Keep the request pending if
        // the LVGL lock is busy, and start its 30 seconds only after success.
        if (screen_wake()) {
            screen_wake_ack(&s_wake_policy);
            screen_idle_activity(&s_idle_policy, esp_timer_get_time());
            ESP_LOGI(TAG, "Screen wake event handled");
        }
        return;
    }
    if (s_screen_off || !sleep_due) return;
    if (!bsp_lvgl_lock(200)) return;
    app_printer_set_suspended(true);
    bsp_display_backlight(0);
    // 熄屏后停掉 LVGL 的 5ms tick 与全部 lv_timer,CPU 才有连续空闲进入 light sleep。
    bsp_lvgl_set_suspended(true);
#if CONFIG_PM_ENABLE
    if (s_awake_lock) esp_pm_lock_release(s_awake_lock);   // 画面已冻结,允许入睡
#endif
    // Network and event checks keep running while the display is suspended.
    s_screen_off = true;
    app_poweroff_poll(true, false);
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "空闲 30s,熄屏");
}

static void screen_off_init(void)
{
    screen_idle_init(&s_idle_policy, esp_timer_get_time());
    s_wake_policy = (screen_wake_state_t){
        .usb_connected = bsp_battery_usb_connected(),
        .print_sequence = app_mqtt_screen_wake_sequence(),
    };
    const esp_timer_create_args_t a = {
        .name = "scr_off", .callback = screen_off_tick, .arg = NULL,
    };
    if (esp_timer_create(&a, &s_scr_timer) == ESP_OK)
        esp_timer_start_periodic(s_scr_timer, 1000ULL * 1000);
}

// true = 这次按键只用于唤醒,不再往下处理。
static bool screen_off_on_key(bsp_btn_ev_t ev)
{
    if (!app_poweroff_activity()) return true;
    screen_idle_activity(&s_idle_policy, esp_timer_get_time());
    if (s_screen_off) {
        screen_wake_request(&s_wake_policy);
        // Consume the entire wake gesture even if rendering resumes later
        // through the pending-request retry rather than this callback.
        if (ev != BSP_BTN_RELEASE) {
            s_swallow_key = true;
            s_swallow_released = false;
        }
        if (screen_wake()) {
            screen_wake_ack(&s_wake_policy);
            screen_idle_activity(&s_idle_policy, esp_timer_get_time());
            ESP_LOGI(TAG, "按键唤醒,亮屏");
        }
        return true;
    }
    if (s_swallow_key) {
        // SINGLE_CLICK follows RELEASE after the button's click timeout.
        // Swallow that delayed action too, so waking does not change pages.
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE) s_swallow_key = false;
        else if (ev == BSP_BTN_RELEASE) s_swallow_released = true;
        else if (ev == BSP_BTN_PRESS && s_swallow_released) {
            s_swallow_key = false;
            s_swallow_released = false;
            return false;
        }
        return true;
    }
    return false;
}

static const demo_entry_t DEMOS[] = {
    { "打印机", app_printer_enter,   app_printer_exit,   app_printer_key },
    { "配网",   app_provision_enter, app_provision_exit, app_provision_key },
    { "切网",   app_wifi_menu_enter, app_wifi_menu_exit, app_wifi_menu_key },
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
    s_menu_scr = ui_pixel_screen_create(APP_PRODUCT_NAME);

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
    if (screen_off_on_key(ev)) return;
    if (!bsp_lvgl_lock(500)) return;

    if (s_active >= 0) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {     // 统一返回
            // 打印机页多机概览时,长按 OK 先被 app_printer 内部消化
            // (详情→概览);只有已在概览页才真正退出到菜单。
            if (s_active == 0 && app_mqtt_machine_count() >= 2 && !app_printer_in_overview()) {
                DEMOS[s_active].key(btn, ev);   // 交回打印机页:返回机器列表
                bsp_lvgl_unlock();
                return;
            }
            DEMOS[s_active].exit();
            enter_menu();
        } else {
            DEMOS[s_active].key(btn, ev);
        }
    } else if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {     // 菜单里上下选择(修:原来只能选中第一项)
            int n = (int)DEMO_COUNT;
            s_sel = (btn == BSP_BTN_UP) ? (s_sel + n - 1) % n : (s_sel + 1) % n;
            menu_refresh();
        } else if (btn == BSP_BTN_OK && s_ok[s_sel]) {
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
#if CONFIG_CAMERA_START_ON_SECOND_PAGE || defined(CAMERA_DIAGNOSTIC_START)
    if (app_mqtt_machine_count() == 1) app_printer_key(BSP_BTN_DOWN, BSP_BTN_CLICK);
#endif
}

static void boot_key_gesture(bool deep_wake, int mv) {
    s_swallow_released = mv >= BSP_BTN_MV_RELEASED_MIN;
    s_swallow_key = deep_wake || !s_swallow_released;
}

void app_main(void) {
#ifdef NOTIFY_TRANSITION_SELFTEST
    extern void notify_transition_selftest(void);
    notify_transition_selftest();
#endif
#if CONFIG_CAMERA_GEOMETRY_SELFTEST
    extern void camera_geometry_selftest(void);
    camera_geometry_selftest();
#endif
    ESP_LOGI(TAG, APP_PRODUCT_NAME " starting on " APP_HARDWARE_NAME);
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Boot wake=%s", wake_cause == ESP_SLEEP_WAKEUP_GPIO ? "button" :
             wake_cause == ESP_SLEEP_WAKEUP_TIMER ? "timer" : "reset");

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,固件无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    pm_init();
    bsp_display_backlight(100);
    screen_off_init();

    s_swallow_key = true;
    s_ok[0] = (bsp_button_init(on_key, NULL) == ESP_OK);   // 页面导航依赖按键
    if (s_ok[0]) boot_key_gesture(wake_cause == ESP_SLEEP_WAKEUP_GPIO, bsp_button_read_mv());
    s_ok[1] = true;                                        // 配网页常驻可用(修:原来进不去)
    s_ok[2] = true;                                        // 切网页:依赖已保存 WiFi 列表
    bsp_audio_init();                                       // 提示音(完成/失败/HMS)
    bsp_battery_init();                                     // 状态栏电量

    // 开机动画(拓竹 logo)→ 直接进打印机监控页
    if (bsp_lvgl_lock(1000)) { app_boot_show(boot_done); bsp_lvgl_unlock(); }

    ESP_LOGI(TAG, "就绪:Button=%d", s_ok[0]);
}

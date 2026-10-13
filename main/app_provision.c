// main/app_provision.c —— 设备端配网(T3:softAP + 网页表单)。
//
// 流程:手机连热点 Bambu-Setup(setup123)→ 手机网页开 192.168.4.1 →
// 填 WiFi + 拓竹 User ID / Token / 打印机 SN → 保存 → 写 NVS → 3 秒后自动重启。
// Token 获取:电脑跑 x2d_badge\weblogin.py 登录拓竹账号,复制 accessToken 粘贴进表单
// (设备端拓竹短信登录留待后续版本)。
#include "app_provision.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "bsp_display.h"      // bsp_lvgl_lock(网页线程更新屏幕要加锁)
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"       // esp_restart()
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "font_zh.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "secrets.h"

static const char *TAG = "provision";

#define PROV_SSID "Bambu-Setup"
#define PROV_PASS "setup123"          // WPA2 至少 8 位

// ------------------------------------------------------------ 网页

static const char *PAGE_HTML =
"<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>拓竹监视器 配网</title><style>"
"body{font-family:system-ui,sans-serif;background:#0b0f16;color:#e6edf3;"
"max-width:420px;margin:0 auto;padding:20px}"
"h2{color:#34d97b;font-size:20px;margin:8px 0}"
"p{color:#6b7a8c;font-size:13px;line-height:1.6}"
"label{display:block;margin:14px 0 4px;font-size:13px;color:#6b7a8c}"
"input{width:100%;box-sizing:border-box;padding:10px;border-radius:6px;"
"border:1px solid #1e2a38;background:#141b25;color:#e6edf3;font-size:15px}"
"button{margin-top:22px;width:100%;padding:12px;border:0;border-radius:6px;"
"background:#00c853;color:#fff;font-size:16px}"
"</style></head><body>"
"<h2>拓竹监视器 · 配网</h2>"
"<p>填入家里 WiFi 与拓竹账号,再逐台添加打印机(同一账号最多 4 台,可起名字)。"
"Token 在电脑跑 weblogin.py 登录后复制。</p>"
"<form method='post' action='/save'>"
"<label>WiFi 名称</label><input name='ssid' required>"
"<label>WiFi 密码</label><input name='pass' type='password' required>"
"<label>拓竹 User ID(纯数字)</label><input name='uid' required>"
"<label>拓竹 Token(accessToken)</label><input name='token' required>"
"<label>1号机 名称(可空,如:书房X1C)</label><input name='name1' maxlength='16'>"
"<label>1号机 序列号</label><input name='sn1' required>"
"<label>2号机 名称(可空)</label><input name='name2' maxlength='16'>"
"<label>2号机 序列号(没第2台留空)</label><input name='sn2'>"
"<label>3号机 名称(可空)</label><input name='name3' maxlength='16'>"
"<label>3号机 序列号(没第3台留空)</label><input name='sn3'>"
"<label>4号机 名称(可空)</label><input name='name4' maxlength='16'>"
"<label>4号机 序列号(没第4台留空)</label><input name='sn4'>"
"<button>保存并重启</button></form></body></html>";

static const char *SAVED_HTML =
"<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"</head><body style='font-family:system-ui;background:#0b0f16;color:#e6edf3;"
"text-align:center;padding-top:60px'>"
"<h2 style='color:#34d97b'>已保存</h2>"
"<p style='color:#6b7a8c'>设备 3 秒后自动重启并开始监控。</p></body></html>";

// ------------------------------------------------------------ 表单解析

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// urldecode,遇到 & 或串尾停止
static void url_decode(const char *in, char *out, int n)
{
    int o = 0;
    for (const char *p = in; *p && *p != '&' && o < n - 1; p++) {
        if (*p == '+') {
            out[o++] = ' ';
        } else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

// 从 urlencoded body 里取字段(%XX 与 + 已解码)
static void form_field(const char *body, const char *key, char *out, int n)
{
    out[0] = 0;
    char pat[16];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (!p) return;
    url_decode(p + strlen(pat), out, n);
}

// ------------------------------------------------------------ HTTP handler

static lv_obj_t *s_scr;       // 配网指引屏
static lv_obj_t *s_status;    // 底部状态行(网页保存后更新)

static void restart_cb(void *arg) { esp_restart(); }

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_HTML, strlen(PAGE_HTML));
}

static esp_err_t save_handler(httpd_req_t *req)
{
    static char buf[1200];
    int total = req->content_len;
    if (total < 0 || total >= (int)sizeof(buf)) total = sizeof(buf) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    buf[got] = 0;

    device_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    form_field(buf, "ssid",  cfg.wifi_ssid,   sizeof(cfg.wifi_ssid));
    form_field(buf, "pass",  cfg.wifi_pass,   sizeof(cfg.wifi_pass));
    form_field(buf, "uid",   cfg.bambu_uid,   sizeof(cfg.bambu_uid));
    form_field(buf, "token", cfg.bambu_token, sizeof(cfg.bambu_token));
    // 多机:sn1..sn4 / name1..name4(1 号机必填,其余可空)
    for (int i = 0; i < CFG_MACHINE_MAX; i++) {
        char key[8];
        snprintf(key, sizeof(key), "sn%d", i + 1);
        form_field(buf, key, cfg.bambu_sn[i], sizeof(cfg.bambu_sn[i]));
        snprintf(key, sizeof(key), "name%d", i + 1);
        form_field(buf, key, cfg.bambu_name[i], sizeof(cfg.bambu_name[i]));
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (!cfg.wifi_ssid[0] || !cfg.wifi_pass[0] || !cfg.bambu_uid[0] ||
        !cfg.bambu_token[0] || !cfg.bambu_sn[0][0]) {
        httpd_resp_send(req, "有必填项为空,请返回补全再提交", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool ok = config_save(&cfg);
    if (ok) config_mark_provisioned();
    ESP_LOGI(TAG, "配置保存%s: ssid=%s uid=%s 机器=%d 台", ok ? "成功" : "失败",
             cfg.wifi_ssid, cfg.bambu_uid, config_machine_count(&cfg));

    if (!ok) {
        httpd_resp_send(req, "NVS 写入失败,请重试", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_send(req, SAVED_HTML, strlen(SAVED_HTML));

    // 屏幕同步提示(网页线程,操作 LVGL 必须加锁)
    if (s_status && bsp_lvgl_lock(500)) {
        lv_label_set_text(s_status, "已保存,即将重启...");
        lv_obj_set_style_text_color(s_status, lv_color_hex(0x34D97B), 0);
        bsp_lvgl_unlock();
    }

    // 3 秒后重启(不阻塞 httpd 线程)
    const esp_timer_create_args_t t = { .name = "prov_restart", .callback = restart_cb };
    esp_timer_handle_t h;
    esp_timer_create(&t, &h);
    esp_timer_start_once(h, 3000000);
    return ESP_OK;
}

static void start_web(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;         // 要收 1KB+ 表单并打印日志,栈别太小
    config.task_priority = 4;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server 启动失败");
        return;
    }
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    ESP_LOGI(TAG, "HTTP server started :80");
}

// ------------------------------------------------------------ 对外

bool app_provision_needed(void)
{
    device_config_t cfg;
    if (!config_load(&cfg)) return true;
    if (cfg.wifi_ssid[0] && cfg.bambu_uid[0] && cfg.bambu_token[0] && cfg.bambu_sn[0][0])
        return false;                                       // NVS 已配好
    // NVS 未配,但 secrets.h 有真值(老机器兼容,刷了硬编码版的直接用)
    if (strncmp(CFG_WIFI_SSID, "YOUR_", 5) != 0 &&
        strncmp(CFG_BAMBU_TOKEN, "paste", 5) != 0)
        return false;
    return true;
}

static bool s_ap_started;

void app_provision_start(void)
{
    if (s_ap_started) return;
    s_ap_started = true;
    ESP_LOGI(TAG, "进入配网模式");

    // NVS 必须先初始化:WiFi 驱动依赖 NVS(否则 esp_wifi_init 报 NOT_INITIALIZED)。
    // 整包从 0x0 烧后 NVS 分区可能无效,需 erase 重建。
    esp_err_t nvs_r = nvs_flash_init();
    if (nvs_r == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_r);

    ESP_ERROR_CHECK(esp_netif_init());
    esp_event_loop_create_default();   // 幂等:已建则忽略返回值
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wcfg = {
        .ap = {
            .ssid = PROV_SSID,
            .ssid_len = (uint8_t)strlen(PROV_SSID),
            .channel = 1,
            .password = PROV_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "softAP 就绪 SSID=%s PASS=%s,手机连后访问 http://192.168.4.1",
             PROV_SSID, PROV_PASS);

    start_web();
}

// ------------------------------------------------------------ 配网指引屏

static lv_obj_t *plabel(lv_obj_t *parent, const char *txt,
                        const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_label_set_text(l, txt);
    return l;
}

// 一条步骤:序号(绿色) + 标题(中文),下一行缩进放值(大号等宽字体)
static void step(lv_obj_t *scr, int y, const char *num, const char *title,
                 const char *val, uint32_t val_col)
{
    lv_obj_t *n = plabel(scr, num, &lv_font_montserrat_14, 0x34D97B);
    lv_obj_set_pos(n, 18, y);
    lv_obj_t *t = plabel(scr, title, &font_zh14, 0xE6EDF3);
    lv_obj_set_pos(t, 36, y);
    if (val) {
        lv_obj_t *v = plabel(scr, val, &lv_font_montserrat_20, val_col);
        lv_obj_set_pos(v, 36, y + 18);
    }
}

void app_provision_enter(void)
{
    if (s_scr) return;
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0B0F16), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    lv_obj_t *title = plabel(s_scr, "设备配网", &font_zh20, 0x34D97B);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *sep = lv_obj_create(s_scr);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(sep, 0, 36);
    lv_obj_set_size(sep, 240, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x1E2A38), 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);

    step(s_scr, 48,  "1", "手机连接热点", PROV_SSID, 0xE6EDF3);
    step(s_scr, 98,  "2", "热点密码",     PROV_PASS, 0xE6EDF3);
    step(s_scr, 148, "3", "手机打开网页",  "192.168.4.1", 0x34D97B);
    step(s_scr, 198, "4", "填写账号并保存", NULL, 0);

    lv_obj_t *hint = plabel(s_scr, "Token 在电脑跑 weblogin.py 登录拓竹账号后复制",
                            &font_zh14, 0x6B7A8C);
    lv_obj_set_width(hint, 220);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 18, 236);

    s_status = plabel(s_scr, "等待配置...", &font_zh14, 0x6B7A8C);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_screen_load(s_scr);
}

void app_provision_exit(void)
{
    if (!s_scr) return;
    lv_obj_delete(s_scr);
    s_scr = NULL;
    s_status = NULL;
}

void app_provision_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    (void)btn; (void)ev;   // 配网页无按键交互
}

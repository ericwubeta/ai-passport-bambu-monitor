// main/app_provision.c —— 设备端配网(softAP + 网页表单)。
//
// 流程:手机连热点 Bambu-Setup(setup123)→ 手机网页开 192.168.4.1 →
// 扫描点选 WiFi(可存多组,开机自动切换)→ 填拓竹账号/打印机 → 保存 → 写 NVS → 重启。
// Token 获取:电脑双击 tools/get-token.bat(或跑 weblogin.py)登录拓竹账号,复制 accessToken。
// 设备端拓竹短信登录留待后续版本。
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
"h3{font-size:15px;margin:24px 0 4px}"
"p{color:#6b7a8c;font-size:13px;line-height:1.6}"
"label{display:block;margin:12px 0 4px;font-size:13px;color:#6b7a8c}"
"input{width:100%;box-sizing:border-box;padding:10px;border-radius:6px;"
"border:1px solid #1e2a38;background:#141b25;color:#e6edf3;font-size:15px}"
"button{margin-top:14px;width:100%;padding:12px;border:0;border-radius:6px;"
"background:#00c853;color:#fff;font-size:16px;cursor:pointer}"
"button.mini{width:auto;padding:7px 14px;margin:0;font-size:13px;background:#2a3546}"
"ul{list-style:none;padding:0;margin:8px 0}"
"li{display:flex;align-items:center;justify-content:space-between;gap:8px;"
"padding:10px 12px;border:1px solid #1e2a38;border-radius:6px;margin:6px 0;"
"background:#141b25;font-size:14px}"
"li.tap{cursor:pointer}"
".r{color:#6b7a8c;font-size:12px;flex-shrink:0}"
"#msg{margin-top:12px;font-size:13px;color:#34d97b;min-height:18px}"
"</style></head><body>"
"<h2>拓竹监视器 · 配网</h2>"
"<h3>1. WiFi(可存 3 组,开机自动切换)</h3>"
"<button onclick='doScan()'>扫描附近 WiFi</button><div id='aps'></div>"
"<label>WiFi 名称(点上方列表自动填,隐藏网络手输)</label><input id='ssid'>"
"<label>WiFi 密码</label><input id='pass' type='password'>"
"<button onclick='addWifi()'>添加 / 更新这组 WiFi</button>"
"<div id='saved'></div>"
"<h3>2. 拓竹账号与打印机</h3>"
"<p>Token 用电脑双击开源项目 tools/get-token.bat 获取后粘贴。"
"只加 WiFi 不改账号时,下面保持原样即可。</p>"
"<label>拓竹 User ID(纯数字)</label><input id='uid' inputmode='numeric'>"
"<label>拓竹 Token(accessToken)</label><input id='token'>"
"<label>1号机 名称(可空,如:书房X1C)</label><input id='name1' maxlength='16'>"
"<label>1号机 序列号</label><input id='sn1'>"
"<label>2号机 名称(可空)</label><input id='name2' maxlength='16'>"
"<label>2号机 序列号(没第2台留空)</label><input id='sn2'>"
"<label>3号机 名称(可空)</label><input id='name3' maxlength='16'>"
"<label>3号机 序列号(没第3台留空)</label><input id='sn3'>"
"<label>4号机 名称(可空)</label><input id='name4' maxlength='16'>"
"<label>4号机 序列号(没第4台留空)</label><input id='sn4'>"
"<button onclick='doSave()'>保存并重启</button>"
"<div id='msg'></div>"
"<script>"
"const $=s=>document.querySelector(s);"
"function msg(t){$('#msg').textContent=t}"
"async function jget(u){const r=await fetch(u);return r.json()}"
"async function jpost(u,b){const r=await fetch(u,{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});"
"return r.json()}"
"function li(text,right,cls){const e=document.createElement('li');"
"const s=document.createElement('span');s.textContent=text;e.appendChild(s);"
"if(right){const r=document.createElement('span');r.className='r';"
"r.textContent=right;e.appendChild(r)}if(cls)e.className=cls;return e}"
"function enc(s){return encodeURIComponent(s)}"
"async function doScan(){msg('扫描中…');"
"try{const d=await jget('/scan');const c=$('#aps');c.innerHTML='';"
"if(!d.aps.length){msg('没扫到 WiFi,再试一次');return}"
"const ul=document.createElement('ul');"
"d.aps.forEach(a=>{const e=li(a.ssid,a.rssi+' dBm','tap');"
"e.onclick=()=>{$('#ssid').value=a.ssid;$('#pass').focus();"
"msg('已选 '+a.ssid+',输密码后点添加')};ul.appendChild(e)});"
"c.appendChild(ul);msg('点选要连的 WiFi')}catch(e){msg('扫描失败,重试一次')}}"
"async function loadSaved(){const d=await jget('/wifis');"
"const c=$('#saved');c.innerHTML='';if(!d.wifis.length){"
"c.innerHTML='<p>还没有保存的 WiFi</p>';return}"
"const ul=document.createElement('ul');"
"d.wifis.forEach(w=>{const e=li(w.ssid,'已保存');"
"const b=document.createElement('button');"
"b.className='mini';b.textContent='删除';"
"b.onclick=async()=>{await jpost('/delwifi','idx='+w.i);loadSaved();"
"msg('已删除 '+w.ssid)};"
"e.appendChild(b);ul.appendChild(e)});"
"c.appendChild(ul)}"
"async function addWifi(){const s=$('#ssid').value.trim();"
"if(!s){msg('先填或点选 WiFi 名称');return}"
"const d=await jpost('/addwifi','ssid='+enc(s)+'&pass='+enc($('#pass').value));"
"msg(d.ok?'已保存 WiFi '+s:(d.err||'保存失败'));loadSaved()}"
"async function loadCfg(){const d=await jget('/cfg');"
"['uid','token','sn1','sn2','sn3','sn4','name1','name2','name3','name4']"
".forEach(k=>{if(d[k])$('#'+k).value=d[k]})}"
"async function doSave(){const f=['uid','token','sn1','sn2','sn3','sn4',"
"'name1','name2','name3','name4'];"
"const b=f.map(k=>k+'='+enc($('#'+k).value)).join('&');"
"const d=await jpost('/save',b);"
"msg(d.ok?'已保存,设备即将重启…':(d.err||'保存失败'))}"
"loadSaved();loadCfg();"
"</script></body></html>";

static const char *SAVED_HTML =
"<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"</head><body style='font-family:system-ui;background:#0b0f16;color:#e6edf3;"
"text-align:center;padding-top:60px'>"
"<h2 style='color:#34d97b'>已保存</h2>"
"<p style='color:#6b7a8c'>设备 3 秒后自动重启并开始监控。</p></body></html>";

// ------------------------------------------------------------ 表单解析
// 所有 HTTP handler 顺序执行(httpd 单任务),共享一个静态缓冲省 RAM。
// 注意:handler 里用 SBUF 作长度,不能用 sizeof(指针)。
#define SBUF 1400
static char s_buf[SBUF];

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

// 读 POST body 到 buf,返回长度(-1 失败)。buf 由调用方提供(静态)。
static int read_body(httpd_req_t *req, char *buf, int cap)
{
    int total = req->content_len;
    if (total < 0 || total >= cap) total = cap - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) return -1;
        got += r;
    }
    buf[got] = 0;
    return got;
}

static void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, json, strlen(json));
}

// JSON 字符串转义(SSID 可能含引号/反斜杠),返回写入 out 的长度
static int json_str(const char *in, char *out, int cap)
{
    int o = 0;
    for (const char *p = in; *p && o < cap - 2; p++) {
        if (*p == '"' || *p == '\\') {
            if (o >= cap - 3) break;
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = 0;
    return o;
}

// ------------------------------------------------------------ HTTP handler

static lv_obj_t *s_scr;       // 配网指引屏
static lv_obj_t *s_status;    // 底部状态行(网页保存后更新)

static void restart_cb(void *arg) { esp_restart(); }

static void schedule_restart(void)
{
    const esp_timer_create_args_t t = { .name = "prov_restart", .callback = restart_cb };
    esp_timer_handle_t h;
    esp_timer_create(&t, &h);
    esp_timer_start_once(h, 3000000);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_HTML, strlen(PAGE_HTML));
}

// 扫描附近 WiFi → JSON {aps:[{ssid,rssi}...]};APSTA 模式下 STA 扫描不影响热点。
static esp_err_t scan_handler(httpd_req_t *req)
{
    char *out = s_buf;
    wifi_scan_config_t sc = { 0 };
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) {
        snprintf(out, SBUF, "{\"err\":\"scan failed %d\"}", (int)e);
        send_json(req, out);
        return ESP_OK;
    }
    uint16_t n = 20;
    wifi_ap_record_t ap[20];
    if (esp_wifi_scan_get_ap_records(&n, ap) != ESP_OK) n = 0;

    int len = snprintf(out, SBUF, "{\"aps\":[");
    for (int i = 0; i < n && len < SBUF - 64; i++) {
        if (!ap[i].ssid[0]) continue;                 // 跳过隐藏网络
        if (strcmp((char *)ap[i].ssid, PROV_SSID) == 0) continue;  // 不列自己
        char esc[66];
        json_str((char *)ap[i].ssid, esc, sizeof(esc));
        len += snprintf(out + len, SBUF - len, "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                        (len > 8) ? "," : "", esc, (int)ap[i].rssi);
    }
    snprintf(out + len, SBUF - len, "]}");
    send_json(req, out);
    return ESP_OK;
}

// 已存 WiFi 列表 → JSON {wifis:[{i,ssid}...]}
static esp_err_t wifis_handler(httpd_req_t *req)
{
    char *out = s_buf;
    device_config_t cfg;
    config_load(&cfg);
    int len = snprintf(out, SBUF, "{\"wifis\":[");
    for (int i = 0; i < CFG_WIFI_MAX; i++) {
        if (!cfg.wifi_ssid[i][0]) continue;
        char esc[66];
        json_str(cfg.wifi_ssid[i], esc, sizeof(esc));
        len += snprintf(out + len, SBUF - len, "%s{\"i\":%d,\"ssid\":\"%s\"}",
                        (len > 10) ? "," : "", i, esc);
    }
    snprintf(out + len, SBUF - len, "]}");
    send_json(req, out);
    return ESP_OK;
}

// 当前账号配置回填表单 → JSON(空字段表示未配置)
static esp_err_t cfg_handler(httpd_req_t *req)
{
    char *out = s_buf;
    device_config_t cfg;
    config_load(&cfg);
    char uid[66], token[514], sn[CFG_MACHINE_MAX][66], nm[CFG_MACHINE_MAX][34];
    json_str(cfg.bambu_uid, uid, sizeof(uid));
    json_str(cfg.bambu_token, token, sizeof(token));
    for (int i = 0; i < CFG_MACHINE_MAX; i++) {
        json_str(cfg.bambu_sn[i], sn[i], sizeof(sn[i]));
        json_str(cfg.bambu_name[i], nm[i], sizeof(nm[i]));
    }
    snprintf(out, SBUF,
             "{\"uid\":\"%s\",\"token\":\"%s\","
             "\"sn1\":\"%s\",\"sn2\":\"%s\",\"sn3\":\"%s\",\"sn4\":\"%s\","
             "\"name1\":\"%s\",\"name2\":\"%s\",\"name3\":\"%s\",\"name4\":\"%s\"}",
             uid, token, sn[0], sn[1], sn[2], sn[3], nm[0], nm[1], nm[2], nm[3]);
    send_json(req, out);
    return ESP_OK;
}

// 添加/更新一组 WiFi:同名覆盖密码,否则占第一个空位
static esp_err_t addwifi_handler(httpd_req_t *req)
{
    static char buf[256];
    static char out[128];
    if (read_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;
    char ssid[33], pass[65];
    form_field(buf, "ssid", ssid, sizeof(ssid));
    form_field(buf, "pass", pass, sizeof(pass));
    if (!ssid[0]) {
        send_json(req, "{\"err\":\"ssid 为空\"}");
        return ESP_OK;
    }
    device_config_t cfg;
    config_load(&cfg);
    int slot = -1;
    for (int i = 0; i < CFG_WIFI_MAX; i++) {
        if (cfg.wifi_ssid[i][0] && strcmp(cfg.wifi_ssid[i], ssid) == 0) { slot = i; break; }
        if (slot < 0 && !cfg.wifi_ssid[i][0]) slot = i;
    }
    if (slot < 0) {
        send_json(req, "{\"err\":\"已存满 3 组,请先删一组\"}");
        return ESP_OK;
    }
    strlcpy(cfg.wifi_ssid[slot], ssid, sizeof(cfg.wifi_ssid[slot]));
    strlcpy(cfg.wifi_pass[slot], pass, sizeof(cfg.wifi_pass[slot]));
    bool ok = config_save(&cfg);
    ESP_LOGI(TAG, "WiFi 写入 %s: 槽%d %s", ok ? "成功" : "失败", slot, ssid);
    snprintf(out, SBUF, "{\"ok\":%d}", ok ? 1 : 0);
    send_json(req, out);
    return ESP_OK;
}

// 删除一组 WiFi(按槽位下标)
static esp_err_t delwifi_handler(httpd_req_t *req)
{
    static char buf[64];
    static char out[64];
    if (read_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;
    char idx[8];
    form_field(buf, "idx", idx, sizeof(idx));
    int i = atoi(idx);
    if (i < 0 || i >= CFG_WIFI_MAX) {
        send_json(req, "{\"err\":\"下标无效\"}");
        return ESP_OK;
    }
    device_config_t cfg;
    config_load(&cfg);
    cfg.wifi_ssid[i][0] = 0;
    cfg.wifi_pass[i][0] = 0;
    bool ok = config_save(&cfg);
    ESP_LOGI(TAG, "WiFi 删除 %s: 槽%d", ok ? "成功" : "失败", i);
    snprintf(out, SBUF, "{\"ok\":%d}", ok ? 1 : 0);
    send_json(req, out);
    return ESP_OK;
}

// 保存拓竹账号(WiFi 由 addwifi 单独管理,这里不动 WiFi 槽)
static esp_err_t save_handler(httpd_req_t *req)
{
    static char buf[1200];
    static char out[128];
    if (read_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;

    device_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    config_load(&cfg);          // 先取旧值:WiFi 槽保留
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

    if (config_wifi_count(&cfg) == 0) {
        send_json(req, "{\"err\":\"请先在上面添加至少一组 WiFi\"}");
        return ESP_OK;
    }
    if (!cfg.bambu_uid[0] || !cfg.bambu_token[0] || !cfg.bambu_sn[0][0]) {
        send_json(req, "{\"err\":\"User ID / Token / 1号机序列号 必填\"}");
        return ESP_OK;
    }

    bool ok = config_save(&cfg);
    if (ok) config_mark_provisioned();
    ESP_LOGI(TAG, "配置保存%s: uid=%s WiFi=%d 组 机器=%d 台", ok ? "成功" : "失败",
             cfg.bambu_uid, config_wifi_count(&cfg), config_machine_count(&cfg));

    if (!ok) {
        send_json(req, "{\"err\":\"NVS 写入失败,请重试\"}");
        return ESP_OK;
    }
    snprintf(out, SBUF, "{\"ok\":1}");
    send_json(req, out);

    // 屏幕同步提示(网页线程,操作 LVGL 必须加锁)
    if (s_status && bsp_lvgl_lock(500)) {
        lv_label_set_text(s_status, "已保存,即将重启...");
        lv_obj_set_style_text_color(s_status, lv_color_hex(0x34D97B), 0);
        bsp_lvgl_unlock();
    }
    schedule_restart();
    return ESP_OK;
}

static void start_web(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;         // 要收表单 + 扫描 JSON,栈别太小
    config.task_priority = 4;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server 启动失败");
        return;
    }
    httpd_uri_t root    = { .uri = "/",        .method = HTTP_GET,  .handler = root_handler,    .user_ctx = NULL };
    httpd_uri_t save    = { .uri = "/save",    .method = HTTP_POST, .handler = save_handler,    .user_ctx = NULL };
    httpd_uri_t scan    = { .uri = "/scan",    .method = HTTP_GET,  .handler = scan_handler,    .user_ctx = NULL };
    httpd_uri_t wifis   = { .uri = "/wifis",   .method = HTTP_GET,  .handler = wifis_handler,   .user_ctx = NULL };
    httpd_uri_t cfg     = { .uri = "/cfg",     .method = HTTP_GET,  .handler = cfg_handler,     .user_ctx = NULL };
    httpd_uri_t addwifi = { .uri = "/addwifi", .method = HTTP_POST, .handler = addwifi_handler, .user_ctx = NULL };
    httpd_uri_t delwifi = { .uri = "/delwifi", .method = HTTP_POST, .handler = delwifi_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &scan);
    httpd_register_uri_handler(server, &wifis);
    httpd_register_uri_handler(server, &cfg);
    httpd_register_uri_handler(server, &addwifi);
    httpd_register_uri_handler(server, &delwifi);
    ESP_LOGI(TAG, "HTTP server started :80");
}

// ------------------------------------------------------------ 对外

bool app_provision_needed(void)
{
    device_config_t cfg;
    if (!config_load(&cfg)) return true;
    if (config_wifi_count(&cfg) > 0 && cfg.bambu_uid[0] &&
        cfg.bambu_token[0] && cfg.bambu_sn[0][0])
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

    // 已配网机器从菜单进配网:WiFi 驱动已在(STA 可能连着家里 WiFi),
    // 不能再 esp_wifi_init。首次开机走完整初始化。两条路径统一在下面分支里处理。
    wifi_mode_t mode;
    bool wifi_inited = (esp_wifi_get_mode(&mode) == ESP_OK);

    wifi_config_t wcfg = {
        .ap = {
            .ssid = PROV_SSID,
            .ssid_len = (uint8_t)strlen(PROV_SSID),
            .channel = 1,               // AP 信道必须 1-13;APSTA 下驱动会自动跟 STA 同信道
            .password = PROV_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (!wifi_inited) {
        // 首次开机:驱动未启动,配置先就位再点火,安全路径。
        esp_netif_create_default_wifi_ap();
        esp_netif_create_default_wifi_sta();   // 扫描走 STA,给它 netif
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wcfg));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        // 已联网机器从菜单进配网:不能在 STA 运行中直接 set_mode(APSTA) ——
        // 驱动会立刻用内部未配置的 AP 参数拉起 softAP,在 ieee80211_hostap_attach
        // 里空指针崩溃(实测 Guru Load fault / rst:0xc)。必须先停机、配好 AP、再启动。
        // STA 凭据在驱动里保留,start 后手动重连即可恢复家里 WiFi/MQTT。
        if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"))
            esp_netif_create_default_wifi_ap();
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wcfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_err_t e = esp_wifi_connect();
        ESP_LOGI(TAG, "热点已就绪,STA 重连: %s", esp_err_to_name(e));
    }
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
    app_provision_start();   // 菜单入口:热点/网页没起就起(幂等)

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
    step(s_scr, 198, "4", "网页选WiFi填账号", NULL, 0);

    lv_obj_t *hint = plabel(s_scr, "Token 用电脑双击 tools/get-token.bat 获取后粘贴",
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

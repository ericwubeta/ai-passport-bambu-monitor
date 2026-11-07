// main/app_provision.c —— 设备端配网(softAP + 网页表单)。
//
// 流程(全程手机,不需要电脑):
//   手机连热点 Bambu-Setup(setup123) → 网页开 192.168.4.1 →
//   1) 选 WiFi 并立即连接(APSTA,边连边保持热点)
//   2) 填拓竹账号手机号 + 短信验证码,设备端直接调用拓竹接口
//   3) 自动取得 accessToken / User ID / 打印机 SN → 写 NVS → 重启
// 高级选项里仍保留手动填 Token/序列号(老用户或接口变动时兜底)。
#include "app_provision.h"

#include <stdio.h>
#include <stdlib.h>      // atoi(delwifi 的槽位下标)
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "app_camera_config.h"
#include "app_wifi.h"
#include "bambu_ca.h"          // api.bambulab.cn 的中间 CA + 根 CA
#include "bsp_display.h"      // bsp_lvgl_lock(网页线程更新屏幕要加锁)
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"       // esp_restart()
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "font_zh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lwip/ip4_addr.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"

#include "nvs_flash.h"

#include "app_secrets.h"

static const char *TAG = "provision";

#define PROV_SSID "Bambu-Setup"
#define PROV_PASS "setup123"          // WPA2 至少 8 位

#define BAMBU_API "https://api.bambulab.cn/v1"
#define API_RSP_MAX 3072              // 登录/绑定接口响应上限(约 1~2KB)
#define TIME_VALID_EPOCH 1700000000   // 2023-11-15;早于此视为未校时,证书校验会失败

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
"input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:6px;"
"border:1px solid #1e2a38;background:#141b25;color:#e6edf3;font-size:15px}"
"button{margin-top:14px;width:100%;padding:12px;border:0;border-radius:6px;"
"background:#00c853;color:#fff;font-size:16px;cursor:pointer}"
"button.mini{width:auto;padding:7px 14px;margin:0;font-size:13px;background:#2a3546}"
"button:disabled{opacity:.5}"
"ul{list-style:none;padding:0;margin:8px 0}"
"li{display:flex;align-items:center;justify-content:space-between;gap:8px;"
"padding:10px 12px;border:1px solid #1e2a38;border-radius:6px;margin:6px 0;"
"background:#141b25;font-size:14px}"
"li.tap{cursor:pointer}"
".r{color:#6b7a8c;font-size:12px;flex-shrink:0}"
"#net{margin-top:10px;padding:10px;border-radius:6px;border:1px solid #1e2a38;"
"background:#141b25;font-size:13px;color:#6b7a8c}"
"#net.ok{color:#34d97b;border-color:#1d6b43}"
"#msg{margin-top:12px;font-size:13px;color:#6b7a8c;min-height:18px}"
"#msg.ok{color:#34d97b}#msg.err{color:#ff6b6b}"
"details{margin-top:22px;border-top:1px solid #1e2a38;padding-top:12px}"
"summary{font-size:13px;color:#6b7a8c;cursor:pointer}"
"</style></head><body>"
"<h2>拓竹监视器 · 配网</h2>"
"<p>全程手机完成，不需要电脑。</p>"
"<h3>1. 连接 WiFi（设备要上网才能登录拓竹）</h3>"
"<button onclick='doScan()'>扫描附近 WiFi</button><div id='aps'></div>"
"<label>WiFi 名称（点上方列表自动填，隐藏网络手输）</label><input id='ssid'>"
"<label>WiFi 密码</label><input id='pass' type='password'>"
"<button id='joinBtn' onclick='joinWifi()'>连接这个 WiFi</button>"
"<div id='net'>未连接，请先完成本步</div>"
"<div id='saved'></div>"
"<h3>2. 拓竹账号（短信登录）</h3>"
"<label>拓竹账号手机号</label><input id='phone' inputmode='tel'>"
"<button id='sendBtn' onclick='sendCode()'>发送验证码</button>"
"<label>短信验证码</label><input id='code' inputmode='numeric' maxlength='6'>"
"<button id='loginBtn' onclick='doLogin()'>登录并完成配置</button>"
"<p>登录成功后自动获取 Token、User ID 和账号下的打印机，无需手动填写。</p>"
"<details><summary>Camera (optional)</summary>"
"<p>Connect the printer and Passport to the same LAN. Configure each printer separately.</p>"
"<label>Printer</label><select id='camidx' onchange='selectCamera()'></select>"
"<label>Printer IP or hostname</label><input id='camhost' maxlength='63' placeholder='192.168.1.100'>"
"<label>LAN access code (8 characters)</label><input id='camcode' type='password' maxlength='8' autocomplete='new-password'>"
"<p id='camcodehint'>Leave blank to keep the saved code.</p>"
"<label>Model / short name (optional)</label><input id='cammodel' maxlength='24'>"
"<label>Camera source</label><select id='cammode'><option value='0'>Auto</option>"
"<option value='1'>JPEG preview</option><option value='2'>A1 / P1 camera</option></select>"
"<label><input id='campair' type='checkbox' style='width:auto'> Pair again after printer replacement / certificate change</label>"
"<button onclick='saveCamera()'>Save camera and restart</button></details>"
"<details><summary>高级：手动填 Token / 序列号</summary>"
"<p>仅在接口异常或需要改单台配置时使用，正常流程不用管这里。</p>"
"<label>拓竹 User ID（纯数字）</label><input id='uid' inputmode='numeric'>"
"<label>拓竹 Token（accessToken）</label><input id='token'>"
"<label>1号机 名称（可空，如：书房X1C）</label><input id='name1' maxlength='16'>"
"<label>1号机 序列号</label><input id='sn1'>"
"<label>2号机 名称（可空）</label><input id='name2' maxlength='16'>"
"<label>2号机 序列号（没第2台留空）</label><input id='sn2'>"
"<label>3号机 名称（可空）</label><input id='name3' maxlength='16'>"
"<label>3号机 序列号（没第3台留空）</label><input id='sn3'>"
"<label>4号机 名称（可空）</label><input id='name4' maxlength='16'>"
"<label>4号机 序列号（没第4台留空）</label><input id='sn4'>"
"<button onclick='doSave()'>保存并重启</button>"
"</details>"
"<div id='msg'></div>"
"<script>"
"const $=s=>document.querySelector(s);"
"let netOk=false;"
"function msg(t,cls){const e=$('#msg');e.textContent=t;e.className=cls||''}"
"async function jget(u){const r=await fetch(u);return r.json()}"
"async function jpost(u,b){const r=await fetch(u,{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});"
"return r.json()}"
"function li(text,right,cls){const e=document.createElement('li');"
"const s=document.createElement('span');s.textContent=text;e.appendChild(s);"
"if(right){const r=document.createElement('span');r.className='r';"
"r.textContent=right;e.appendChild(r)}if(cls)e.className=cls;return e}"
"function enc(s){return encodeURIComponent(s)}"
"async function pollNet(){try{const d=await jget('/netstatus');netOk=!!d.sta;"
"const n=$('#net');n.className=netOk?'ok':'';"
"n.textContent=netOk?('已联网：'+d.ssid+' ('+d.ip+')'):'未连接，请先完成本步';"
"return netOk}catch(e){return false}}"
"async function doScan(){msg('扫描中…');"
"try{const d=await jget('/scan');const c=$('#aps');c.innerHTML='';"
"if(!d.aps.length){msg('没扫到 WiFi，再试一次','err');return}"
"const ul=document.createElement('ul');"
"d.aps.forEach(a=>{const e=li(a.ssid,a.rssi+' dBm','tap');"
"e.onclick=()=>{$('#ssid').value=a.ssid;$('#pass').focus();"
"msg('已选 '+a.ssid+'，输密码后点连接')};ul.appendChild(e)});"
"c.appendChild(ul);msg('点选要连的 WiFi')}catch(e){msg('扫描失败，重试一次','err')}}"
"function waitNet(){return new Promise(res=>{let t=0;"
"const iv=setInterval(async()=>{t+=2;const ok=await pollNet();"
"if(ok||t>=24){clearInterval(iv);res(ok)}},2000)})}"
"async function joinWifi(){const s=$('#ssid').value.trim();"
"if(!s){msg('先填或点选 WiFi 名称','err');return}"
"const b=$('#joinBtn');b.disabled=true;"
"msg('正在连接 WiFi…热点可能短暂切信道，手机掉线请重连 Bambu-Setup');"
"try{const d=await jpost('/joinwifi','ssid='+enc(s)+'&pass='+enc($('#pass').value));"
"if(!d.ok){msg(d.err||'连接失败','err');b.disabled=false;return}}"
"catch(e){msg('热点切换导致请求中断，请重连 Bambu-Setup 后查看网络状态','err')}"
"const ok=await waitNet();b.disabled=false;"
"if(ok){msg('WiFi 已连接，继续第 2 步','ok');loadSaved()}"
"else msg('WiFi 还没连上，检查密码后重试','err')}"
"async function sendCode(){const p=$('#phone').value.trim();"
"if(!/^[0-9]{6,15}$/.test(p)){msg('请输入正确的手机号','err');return}"
"if(!netOk){msg('请先完成第 1 步：连接 WiFi','err');return}"
"const b=$('#sendBtn');b.disabled=true;msg('正在向拓竹发送验证码…');"
"try{const d=await jpost('/sendcode','phone='+enc(p));"
"if(d.ok){msg('验证码已发送，请查收短信','ok');$('#code').focus()}"
"else msg('发送失败：'+(d.err||'未知错误'),'err')}"
"catch(e){msg('发送失败，请重试','err')}b.disabled=false}"
"async function doLogin(){const p=$('#phone').value.trim();"
"const c=$('#code').value.trim();"
"if(!/^[0-9]{6,15}$/.test(p)){msg('手机号不对','err');return}"
"if(!/^[0-9]{6}$/.test(c)){msg('请输入 6 位验证码','err');return}"
"if(!netOk){msg('请先完成第 1 步：连接 WiFi','err');return}"
"const b=$('#loginBtn');b.disabled=true;msg('正在登录并读取打印机，请稍候…');"
"try{const d=await jpost('/login','account='+enc(p)+'&code='+enc(c));"
"if(d.ok){let t='配置完成：'+(d.count||0)+' 台打印机';"
"if(d.names)t+='（'+d.names+'）';msg(t+'，设备 5 秒后重启…','ok')}"
"else b.disabled=false,msg('登录失败：'+(d.err||'未知错误'),'err')}"
"catch(e){b.disabled=false;msg('登录超时，请重试','err')}}"
"async function loadSaved(){const d=await jget('/wifis');"
"const c=$('#saved');c.innerHTML='';if(!d.wifis.length)return;"
"const ul=document.createElement('ul');"
"d.wifis.forEach(w=>{const e=li(w.ssid);"
"const b=document.createElement('button');"
"b.className='mini';b.textContent='删除';"
"b.onclick=async()=>{await jpost('/delwifi','idx='+w.i);loadSaved();"
"msg('已删除 '+w.ssid+'，重启后不再连接')};"
"e.appendChild(b);ul.appendChild(e)});"
"c.appendChild(ul)}"
"async function loadCfg(){const d=await jget('/cfg');"
"['uid','token','sn1','sn2','sn3','sn4','name1','name2','name3','name4']"
".forEach(k=>{if(d[k])$('#'+k).value=d[k]})}"
"async function doSave(){const f=['uid','token','sn1','sn2','sn3','sn4',"
"'name1','name2','name3','name4'];"
"const b=f.map(k=>k+'='+enc($('#'+k).value)).join('&');"
"const d=await jpost('/save',b);"
"msg(d.ok?'已保存，设备即将重启…':(d.err||'保存失败'),d.ok?'ok':'err')}"
"let cameras=[];async function loadCameras(){try{const d=await jget('/camera');cameras=d.machines||[];"
"const sel=$('#camidx');sel.textContent='';cameras.forEach(c=>{const o=document.createElement('option');"
"o.value=c.index;o.textContent=c.name||('Printer '+(c.index+1));sel.appendChild(o)});selectCamera()}catch(e){}}"
"function selectCamera(){const c=cameras.find(c=>c.index===Number($('#camidx').value));if(!c)return;"
"$('#camhost').value=c.host||'';$('#cammodel').value=c.model||'';$('#cammode').value=String(c.mode||0);"
"$('#camcode').value='';$('#campair').checked=false;$('#camcodehint').textContent=c.has_code?"
"'Code saved. Leave blank to keep it.':'Enter the LAN access code shown on the printer.'}"
"async function saveCamera(){const b='idx='+enc($('#camidx').value)+'&host='+enc($('#camhost').value.trim())"
"+'&code='+enc($('#camcode').value)+'&model='+enc($('#cammodel').value)+'&mode='+enc($('#cammode').value)"
"+'&pair='+($('#campair').checked?'1':'0');try{const d=await jpost('/camera',b);"
"msg(d.ok?'Camera saved. Restarting...':(d.err||'Camera save failed'),d.ok?'ok':'err')}"
"catch(e){msg('Camera save failed','err')}}"
"pollNet();setInterval(pollNet,3000);loadSaved();loadCfg();loadCameras();"
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

// ------------------------------------------------------------ 联网 / 拓竹 API

// 按 UTF-8 边界截断复制(打印机名可能是中文,不能从半个汉字切开)
static void utf8_copy(char *dst, int cap, const char *src)
{
    if (!src) src = "";
    int n = (int)strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static bool is_digits(const char *s, int min, int max)
{
    int n = (int)strlen(s);
    if (n < min || n > max) return false;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9') return false;
    return true;
}

// STA 是否已拿到 IP;ip 可为 NULL
static bool sta_ip(char *ip, int n)
{
    esp_netif_t *nf = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!nf || esp_netif_get_ip_info(nf, &info) != ESP_OK || info.ip.addr == 0) {
        if (ip && n > 0) snprintf(ip, n, "0.0.0.0");
        return false;
    }
    if (ip && n > 0) snprintf(ip, n, IPSTR, IP2STR(&info.ip));
    return true;
}

// TLS 证书校验依赖系统时间;首次配网没有 app_wifi 启动的 SNTP,这里补一次。
static bool wait_time_sync(uint32_t ms)
{
    if (time(NULL) > TIME_VALID_EPOCH) return true;

    static bool s_sntp;
    if (!s_sntp) {
        esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp1.aliyun.com");
        // app_wifi 路径可能已启动过 SNTP,重复 init 会返回 INVALID_STATE,忽略即可
        esp_netif_sntp_init(&c);
        s_sntp = true;
        ESP_LOGI(TAG, "SNTP 启动(配网校时)");
    }

    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (time(NULL) <= TIME_VALID_EPOCH) {
        if (xTaskGetTickCount() > end) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "校时完成,当前 time=%ld", (long)time(NULL));
    return true;
}

static bool ensure_online(char *err, int n)
{
    char ip[20];
    if (!sta_ip(ip, sizeof(ip))) {
        snprintf(err, n, "请先完成第 1 步：连接 WiFi");
        return false;
    }
    if (!wait_time_sync(10000)) {
        snprintf(err, n, "WiFi 已连但校时失败，请确认该 WiFi 能正常上网");
        return false;
    }
    ESP_LOGI(TAG, "联网就绪 ip=%s time=%ld heap=%u", ip, (long)time(NULL),
             (unsigned)esp_get_free_heap_size());

    // DNS 诊断:解析失败时 TLS 连接会一直等到超时
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int64_t t0 = esp_timer_get_time();
    int gr = getaddrinfo("api.bambulab.cn", "443", &hints, &res);
    if (gr == 0 && res) {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        ESP_LOGD(TAG, "DNS 解析成功 %s:%u, 耗时 %lld ms",
                 inet_ntoa(sa->sin_addr), (unsigned)ntohs(sa->sin_port),
                 (long long)((esp_timer_get_time() - t0) / 1000));
        freeaddrinfo(res);
    } else {
        ESP_LOGE(TAG, "DNS 解析失败: %d, 耗时 %lld ms", gr,
                 (long long)((esp_timer_get_time() - t0) / 1000));
    }
    return true;
}

typedef struct {
    char *buf;
    int cap;
    int len;
} http_rsp_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    http_rsp_t *r = (http_rsp_t *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && r && r->buf && e->data_len > 0) {
        int n = e->data_len;
        if (r->len + n > r->cap - 1) n = r->cap - 1 - r->len;
        if (n > 0) {
            memcpy(r->buf + r->len, e->data, n);
            r->len += n;
            r->buf[r->len] = 0;
        }
    }
    return ESP_OK;
}

// 调用拓竹接口;body 非空=POST,空=GET。返回 HTTP 状态码,失败 -1。
static int http_call(const char *path, const char *body, const char *token,
                     char *rsp, int cap)
{
    char url[96];
    snprintf(url, sizeof(url), "%s%s", BAMBU_API, path);
    if (rsp && cap > 0) rsp[0] = 0;
    http_rsp_t r = { rsp, cap, 0 };

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 512,
        .buffer_size_tx = 512,
        // 显式指定 Bambu 的中间 CA + 根 CA,不依赖全局证书包
        .cert_pem = BAMBU_CA_PEM,
        .event_handler = http_evt,
        .user_data = &r,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return -1;

    if (body) {
        esp_http_client_set_method(h, HTTP_METHOD_POST);
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_post_field(h, body, (int)strlen(body));
    } else {
        esp_http_client_set_method(h, HTTP_METHOD_GET);
    }
    if (token && token[0]) {
        char auth[288];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(h, "Authorization", auth);
    }

    int64_t t0 = esp_timer_get_time();
    ESP_LOGD(TAG, "TLS 前: free=%u largest=%u min=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned)esp_get_minimum_free_heap_size());
    esp_err_t e = esp_http_client_perform(h);
    int status = (e == ESP_OK) ? esp_http_client_get_status_code(h) : -1;
    ESP_LOGI(TAG, "%s -> HTTP %d, 耗时 %lld ms", path, status,
             (long long)((esp_timer_get_time() - t0) / 1000));
    if (e != ESP_OK) ESP_LOGE(TAG, "HTTP %s 失败: %s", path, esp_err_to_name(e));
    esp_http_client_cleanup(h);
    return status;
}

static const char *json_gets(cJSON *o, const char *key)
{
    if (!o) return NULL;
    cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

// 取字符串或数字标量(uid 有时是数字)
static bool json_scalar(cJSON *o, const char *key, char *out, int n)
{
    if (!o) return false;
    cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(i) && i->valuestring && i->valuestring[0]) {
        strlcpy(out, i->valuestring, n);
        return true;
    }
    if (cJSON_IsNumber(i)) {
        snprintf(out, n, "%.0f", i->valuedouble);
        return out[0] != 0;
    }
    return false;
}

// 从原始 JSON 前缀里直接抠 "uid" 数字(不建树,避免 preferences 大响应吃堆)
static bool uid_from_raw(const char *s, char *out, int n)
{
    const char *p = s ? strstr(s, "\"uid\"") : NULL;
    if (!p) return false;
    p += 5;
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    int o = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && o < n - 1) out[o++] = *p++;
    } else {
        while (*p >= '0' && *p <= '9' && o < n - 1) out[o++] = *p++;
    }
    out[o] = 0;
    return o > 0;
}

// 从 API 错误响应里提取可读信息
static void api_err(const char *rsp, int status, const char *fallback,
                    char *out, int n)
{
    cJSON *j = (rsp && rsp[0] == '{') ? cJSON_Parse(rsp) : NULL;
    const char *m = json_gets(j, "error");
    if (!m) m = json_gets(j, "errmessage");
    if (!m) m = json_gets(j, "msg");
    if (!m) m = json_gets(j, "message");
    if (m && m[0]) snprintf(out, n, "%s", m);
    else if (status > 0) snprintf(out, n, "拓竹接口 HTTP %d", status);
    else snprintf(out, n, "%s", fallback);
    cJSON_Delete(j);
}

// 保存一组 WiFi,返回槽位;-2 已存满,-1 写入失败
static int wifi_store(const char *ssid, const char *pass)
{
    device_config_t cfg;
    config_load(&cfg);
    int slot = -1;
    for (int i = 0; i < CFG_WIFI_MAX; i++) {
        if (cfg.wifi_ssid[i][0] && strcmp(cfg.wifi_ssid[i], ssid) == 0) { slot = i; break; }
        if (slot < 0 && !cfg.wifi_ssid[i][0]) slot = i;
    }
    if (slot < 0) return -2;
    strlcpy(cfg.wifi_ssid[slot], ssid, sizeof(cfg.wifi_ssid[slot]));
    strlcpy(cfg.wifi_pass[slot], pass, sizeof(cfg.wifi_pass[slot]));
    return config_save(&cfg) ? slot : -1;
}

// ------------------------------------------------------------ HTTP handler

static lv_obj_t *s_scr;       // 配网指引屏
static lv_obj_t *s_status;    // 底部状态行(网页保存后更新)

static void restart_cb(void *arg) { esp_restart(); }

static void schedule_restart(uint32_t ms)
{
    static bool s_armed;
    if (s_armed) return;
    s_armed = true;
    const esp_timer_create_args_t t = { .name = "prov_restart", .callback = restart_cb };
    esp_timer_handle_t h;
    esp_timer_create(&t, &h);
    esp_timer_start_once(h, (uint64_t)ms * 1000);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // 设备端页面常改,禁止浏览器缓存,避免手机一直拿到旧版表单/脚本
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
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

// 只保存一组 WiFi(不立即连接)
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
    int slot = wifi_store(ssid, pass);
    if (slot == -2) {
        send_json(req, "{\"err\":\"已存满 3 组,请先删一组\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "WiFi 写入 %s: 槽%d %s", slot >= 0 ? "成功" : "失败", slot, ssid);
    snprintf(out, SBUF, "{\"ok\":%d}", slot >= 0 ? 1 : 0);
    send_json(req, out);
    return ESP_OK;
}

// 保存并立即连接一组 WiFi(AP 热点保持,手机可继续访问网页)
static esp_err_t joinwifi_handler(httpd_req_t *req)
{
    static char buf[256];
    if (read_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;
    char ssid[33], pass[65];
    form_field(buf, "ssid", ssid, sizeof(ssid));
    form_field(buf, "pass", pass, sizeof(pass));
    if (!ssid[0]) {
        send_json(req, "{\"err\":\"先填或点选 WiFi 名称\"}");
        return ESP_OK;
    }

    int slot = wifi_store(ssid, pass);
    if (slot == -2) {
        send_json(req, "{\"err\":\"已存满 3 组，请先在下方删一组\"}");
        return ESP_OK;
    }
    if (slot < 0) {
        send_json(req, "{\"err\":\"NVS 写入失败，请重试\"}");
        return ESP_OK;
    }

    // 已联网时切网会触发 app_wifi 的自动重连;暂停它,避免把刚设的新网络顶掉
    app_wifi_pause_reconnect(20000);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    esp_wifi_disconnect();                       // 未连接时返回错误,忽略
    esp_err_t e1 = esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_err_t e2 = esp_wifi_connect();
    ESP_LOGI(TAG, "WiFi 立即连接 %s: set=%s connect=%s",
             ssid, esp_err_to_name(e1), esp_err_to_name(e2));
    if (e1 != ESP_OK) {
        send_json(req, "{\"err\":\"WiFi 参数设置失败\"}");
        return ESP_OK;
    }
    send_json(req, "{\"ok\":1}");
    return ESP_OK;
}

// 当前 STA 联网状态 → JSON {sta,ip,ssid,rssi}
static esp_err_t netstatus_handler(httpd_req_t *req)
{
    char ip[20];
    bool up = sta_ip(ip, sizeof(ip));
    char ssid[66] = "";
    int rssi = 0;
    wifi_ap_record_t ap;
    if (up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.ssid[0]) {
        json_str((char *)ap.ssid, ssid, sizeof(ssid));
        rssi = ap.rssi;
    }
    snprintf(s_buf, SBUF, "{\"sta\":%d,\"ip\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d}",
             up ? 1 : 0, ip, ssid, rssi);
    send_json(req, s_buf);
    return ESP_OK;
}

// 连通性自检:不登录、不发短信,只验证到 api.bambulab.cn 的 HTTPS 通路
static esp_err_t nettest_handler(httpd_req_t *req)
{
    char err[96], ip[20];
    if (!ensure_online(err, sizeof(err))) {
        snprintf(s_buf, SBUF, "{\"err\":\"%s\"}", err);
        send_json(req, s_buf);
        return ESP_OK;
    }
    sta_ip(ip, sizeof(ip));
    int64_t t0 = esp_timer_get_time();
    int status = http_call("", NULL, NULL, NULL, 0);
    snprintf(s_buf, SBUF,
             "{\"ip\":\"%s\",\"time\":%ld,\"heap\":%u,\"status\":%d,\"ms\":%lld}",
             ip, (long)time(NULL), (unsigned)esp_get_free_heap_size(), status,
             (long long)((esp_timer_get_time() - t0) / 1000));
    send_json(req, s_buf);
    return ESP_OK;
}

// 发送拓竹短信验证码
static esp_err_t sendcode_handler(httpd_req_t *req)
{
    static char buf[128];
    if (read_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;
    char phone[20];
    form_field(buf, "phone", phone, sizeof(phone));
    if (!is_digits(phone, 6, 15)) {
        send_json(req, "{\"err\":\"手机号格式不对\"}");
        return ESP_OK;
    }

    char err[96];
    if (!ensure_online(err, sizeof(err))) {
        snprintf(s_buf, SBUF, "{\"err\":\"%s\"}", err);
        send_json(req, s_buf);
        return ESP_OK;
    }

    char body[96];
    snprintf(body, sizeof(body), "{\"phone\":\"%s\",\"type\":\"codeLogin\"}", phone);
    char *rsp = malloc(API_RSP_MAX);
    if (!rsp) {
        send_json(req, "{\"err\":\"内存不足，请重启设备\"}");
        return ESP_OK;
    }
    int status = http_call("/user-service/user/sendsmscode", body, NULL, rsp, API_RSP_MAX);
    ESP_LOGI(TAG, "sendsmscode HTTP %d", status);
    if (status == 200) {
        send_json(req, "{\"ok\":1}");
    } else {
        api_err(rsp, status, "发送失败，请检查 WiFi 网络", err, sizeof(err));
        snprintf(s_buf, SBUF, "{\"err\":\"%s\"}", err);
        send_json(req, s_buf);
    }
    free(rsp);
    return ESP_OK;
}

// 短信登录 → 自动取 token/uid/打印机 → 写 NVS → 重启
static esp_err_t login_handler(httpd_req_t *req)
{
    static char buf[128];
    if (read_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;
    char account[20], code[12], err[96];
    form_field(buf, "account", account, sizeof(account));
    form_field(buf, "code", code, sizeof(code));
    if (!is_digits(account, 6, 15) || !is_digits(code, 6, 6)) {
        send_json(req, "{\"err\":\"手机号或验证码格式不对\"}");
        return ESP_OK;
    }
    if (!ensure_online(err, sizeof(err))) {
        snprintf(s_buf, SBUF, "{\"err\":\"%s\"}", err);
        send_json(req, s_buf);
        return ESP_OK;
    }

    char *rsp = malloc(API_RSP_MAX);
    if (!rsp) {
        send_json(req, "{\"err\":\"内存不足，请重启设备\"}");
        return ESP_OK;
    }

    char body[96];
    snprintf(body, sizeof(body), "{\"account\":\"%s\",\"code\":\"%s\"}", account, code);
    int status = http_call("/user-service/user/login", body, NULL, rsp, API_RSP_MAX);
    ESP_LOGI(TAG, "login HTTP %d", status);
    if (status != 200) {
        api_err(rsp, status, "登录失败，验证码可能已过期", err, sizeof(err));
        snprintf(s_buf, SBUF, "{\"err\":\"%s\"}", err);
        send_json(req, s_buf);
        free(rsp);
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(rsp);
    if (!root) {
        send_json(req, "{\"err\":\"登录响应解析失败\"}");
        free(rsp);
        return ESP_OK;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");

    char token[257] = "", uid[33] = "";
    const char *tk = json_gets(root, "accessToken");
    if (!tk) tk = json_gets(data, "accessToken");
    if (tk) strlcpy(token, tk, sizeof(token));

    const char *uid_keys[] = { "uid", "userId", "user_id", "userNo",
                               "memberId", "accountId", "openid", "sub" };
    for (size_t i = 0; i < sizeof(uid_keys) / sizeof(uid_keys[0]) && !uid[0]; i++) {
        if (!json_scalar(root, uid_keys[i], uid, sizeof(uid)))
            json_scalar(data, uid_keys[i], uid, sizeof(uid));
    }
    cJSON_Delete(root);

    if (!token[0]) {
        send_json(req, "{\"err\":\"登录成功但没拿到 accessToken\"}");
        free(rsp);
        return ESP_OK;
    }

    // 登录响应常不带 uid,再从 preferences 接口取。
    // 该响应约 3.6KB,这里只收前 512 字节(uid 在开头),其余丢弃,省堆也避免截断解析失败。
    if (!uid[0]) {
        status = http_call("/design-user-service/my/preference", NULL, token, rsp, 512);
        uid_from_raw(rsp, uid, sizeof(uid));
        ESP_LOGI(TAG, "preference HTTP %d, uid=%s", status, uid);
    }
    if (!uid[0]) {
        send_json(req, "{\"err\":\"没取到 User ID，请重试\"}");
        free(rsp);
        return ESP_OK;
    }

    // 账号绑定的打印机
    status = http_call("/iot-service/api/user/bind", NULL, token, rsp, API_RSP_MAX);
    cJSON *bind = (status == 200) ? cJSON_Parse(rsp) : NULL;
    cJSON *devs = bind ? cJSON_GetObjectItemCaseSensitive(bind, "devices") : NULL;
    if (!devs && bind) {
        cJSON *bd = cJSON_GetObjectItemCaseSensitive(bind, "data");
        devs = cJSON_GetObjectItemCaseSensitive(bd, "devices");
    }
    if (!devs || !cJSON_IsArray(devs)) {
        cJSON_Delete(bind);
        send_json(req, "{\"err\":\"读取打印机列表失败\"}");
        free(rsp);
        return ESP_OK;
    }

    device_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    config_load(&cfg);                 // 保留 WiFi 槽
    strlcpy(cfg.bambu_uid, uid, sizeof(cfg.bambu_uid));
    strlcpy(cfg.bambu_token, token, sizeof(cfg.bambu_token));

    char names[128] = "";
    int count = 0;
    cJSON *d = NULL;
    cJSON_ArrayForEach(d, devs) {
        const char *sn = json_gets(d, "dev_id");
        if (!sn || !sn[0]) continue;
        if (count >= CFG_MACHINE_MAX) break;
        strlcpy(cfg.bambu_sn[count], sn, sizeof(cfg.bambu_sn[count]));
        const char *nm = json_gets(d, "name");
        utf8_copy(cfg.bambu_name[count], sizeof(cfg.bambu_name[count]), nm);
        if (count > 0) strlcat(names, ",", sizeof(names));
        if (nm && nm[0]) strlcat(names, nm, sizeof(names));
        else {
            char fallback[24];
            snprintf(fallback, sizeof(fallback), "%d号机", count + 1);
            strlcat(names, fallback, sizeof(names));
        }
        count++;
    }
    cJSON_Delete(bind);
    free(rsp);

    if (count == 0) {
        send_json(req, "{\"err\":\"账号下没有打印机，请先在拓竹 App 绑定\"}");
        return ESP_OK;
    }
    for (int i = count; i < CFG_MACHINE_MAX; i++) {
        cfg.bambu_sn[i][0] = 0;
        cfg.bambu_name[i][0] = 0;
    }
    if (config_wifi_count(&cfg) == 0) {
        send_json(req, "{\"err\":\"请先完成第 1 步：连接 WiFi\"}");
        return ESP_OK;
    }

    bool ok = config_save(&cfg);
    if (ok) config_mark_provisioned();
    ESP_LOGI(TAG, "短信登录配置%s: uid=%s 机器=%d 台 WiFi=%d 组",
             ok ? "成功" : "失败", cfg.bambu_uid, count, config_wifi_count(&cfg));
    if (!ok) {
        send_json(req, "{\"err\":\"NVS 写入失败，请重试\"}");
        return ESP_OK;
    }

    char esc_names[160];
    json_str(names, esc_names, sizeof(esc_names));
    snprintf(s_buf, SBUF, "{\"ok\":1,\"count\":%d,\"names\":\"%s\"}", count, esc_names);
    send_json(req, s_buf);

    if (s_status && bsp_lvgl_lock(500)) {
        lv_label_set_text(s_status, "已登录，即将重启...");
        lv_obj_set_style_text_color(s_status, lv_color_hex(0x34D97B), 0);
        bsp_lvgl_unlock();
    }
    schedule_restart(5000);
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
    if (!idx[0]) {              // 缺参数时 atoi("")=0 会误删槽 0,必须挡住
        send_json(req, "{\"err\":\"缺少 idx\"}");
        return ESP_OK;
    }
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
    schedule_restart(3000);
    return ESP_OK;
}

static esp_err_t camera_get_handler(httpd_req_t *req) {
    device_config_t devices;
    config_load_printers(&devices);
    int used = snprintf(s_buf, SBUF, "{\"machines\":[");
    bool first = true;
    for (int i = 0; i < CFG_MACHINE_MAX; i++) {
        if (!devices.bambu_sn[i][0]) continue;
        camera_config_t camera;
        camera_config_load(devices.bambu_sn[i], &camera);
        char name[36], host[130], model[52];
        json_str(devices.bambu_name[i], name, sizeof(name));
        json_str(camera.host, host, sizeof(host));
        json_str(camera.model, model, sizeof(model));
        int n = snprintf(s_buf+used, SBUF-used,
            "%s{\"index\":%d,\"name\":\"%s\",\"host\":\"%s\",\"model\":\"%s\",\"mode\":%u,\"has_code\":%s}",
            first ? "" : ",", i, name, host, model, camera.transport, camera.access_code[0] ? "true" : "false");
        if (n < 0 || n >= SBUF-used-3) return ESP_FAIL;
        used += n; first = false;
    }
    snprintf(s_buf+used, SBUF-used, "]}");
    send_json(req, s_buf);
    return ESP_OK;
}

static esp_err_t camera_save_handler(httpd_req_t *req) {
    char body[512], index[8], host[64], code[34], model[100], mode[8], pair[4];
    if (req->content_len >= sizeof(body) || read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;
    form_field(body, "idx", index, sizeof(index));
    form_field(body, "host", host, sizeof(host));
    form_field(body, "code", code, sizeof(code));
    form_field(body, "model", model, sizeof(model));
    form_field(body, "mode", mode, sizeof(mode));
    form_field(body, "pair", pair, sizeof(pair));
    if (strlen(index) != 1 || index[0] < '0' || index[0] >= '0'+CFG_MACHINE_MAX ||
        strlen(mode) != 1 || mode[0] < '0' || mode[0] > '2') {
        send_json(req, "{\"err\":\"Invalid printer or camera source\"}"); return ESP_OK;
    }
    for (const char *p = host; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) {
            send_json(req, "{\"err\":\"Enter a printer IP or hostname, without a URL or port\"}"); return ESP_OK;
        }
    }
    device_config_t devices;
    config_load_printers(&devices);
    const char *serial = devices.bambu_sn[index[0]-'0'];
    if (!serial[0]) { send_json(req, "{\"err\":\"Configure the printer account first\"}"); return ESP_OK; }
    camera_config_t camera;
    camera_config_load(serial, &camera);
    if (strcmp(host, camera.host) || (code[0] && strcmp(code, camera.access_code)) || pair[0] == '1')
        memset(camera.certificate_sha256, 0, sizeof(camera.certificate_sha256));
    strlcpy(camera.host, host, sizeof(camera.host));
    utf8_copy(camera.model, sizeof(camera.model), model);
    camera.transport = mode[0]-'0';
    if (code[0]) {
        if (strlen(code) != 8) { send_json(req, "{\"err\":\"LAN access code must have 8 characters\"}"); return ESP_OK; }
        for (int i = 0; i < 8; i++) if ((unsigned char)code[i] < 33 || (unsigned char)code[i] > 126) {
            send_json(req, "{\"err\":\"Invalid LAN access code\"}"); return ESP_OK;
        }
        strlcpy(camera.access_code, code, sizeof(camera.access_code));
    }
    if (host[0] && !camera.access_code[0]) { send_json(req, "{\"err\":\"Enter the printer LAN access code\"}"); return ESP_OK; }
    if (!camera_config_save(serial, &camera)) { send_json(req, "{\"err\":\"Camera configuration could not be saved\"}"); return ESP_OK; }
    send_json(req, "{\"ok\":1}");
    schedule_restart(3000);
    return ESP_OK;
}

static void start_web(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;         // HTTP 表单 + TLS 握手(12KB 会挤占 TLS 堆内存)
    config.max_uri_handlers = 15;
    config.task_priority = 4;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server 启动失败");
        return;
    }
    httpd_uri_t root      = { .uri = "/",          .method = HTTP_GET,  .handler = root_handler,      .user_ctx = NULL };
    httpd_uri_t save      = { .uri = "/save",      .method = HTTP_POST, .handler = save_handler,      .user_ctx = NULL };
    httpd_uri_t scan      = { .uri = "/scan",      .method = HTTP_GET,  .handler = scan_handler,      .user_ctx = NULL };
    httpd_uri_t wifis     = { .uri = "/wifis",     .method = HTTP_GET,  .handler = wifis_handler,     .user_ctx = NULL };
    httpd_uri_t cfg       = { .uri = "/cfg",       .method = HTTP_GET,  .handler = cfg_handler,       .user_ctx = NULL };
    httpd_uri_t addwifi   = { .uri = "/addwifi",   .method = HTTP_POST, .handler = addwifi_handler,   .user_ctx = NULL };
    httpd_uri_t delwifi   = { .uri = "/delwifi",   .method = HTTP_POST, .handler = delwifi_handler,   .user_ctx = NULL };
    httpd_uri_t joinwifi  = { .uri = "/joinwifi",  .method = HTTP_POST, .handler = joinwifi_handler,  .user_ctx = NULL };
    httpd_uri_t netstatus = { .uri = "/netstatus", .method = HTTP_GET,  .handler = netstatus_handler, .user_ctx = NULL };
    httpd_uri_t sendcode  = { .uri = "/sendcode",  .method = HTTP_POST, .handler = sendcode_handler,  .user_ctx = NULL };
    httpd_uri_t login     = { .uri = "/login",     .method = HTTP_POST, .handler = login_handler,     .user_ctx = NULL };
    httpd_uri_t nettest   = { .uri = "/nettest",   .method = HTTP_GET,  .handler = nettest_handler,   .user_ctx = NULL };
    httpd_uri_t camera_get = { .uri = "/camera", .method = HTTP_GET, .handler = camera_get_handler };
    httpd_uri_t camera_save = { .uri = "/camera", .method = HTTP_POST, .handler = camera_save_handler };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &scan);
    httpd_register_uri_handler(server, &wifis);
    httpd_register_uri_handler(server, &cfg);
    httpd_register_uri_handler(server, &addwifi);
    httpd_register_uri_handler(server, &delwifi);
    httpd_register_uri_handler(server, &joinwifi);
    httpd_register_uri_handler(server, &netstatus);
    httpd_register_uri_handler(server, &sendcode);
    httpd_register_uri_handler(server, &login);
    httpd_register_uri_handler(server, &nettest);
    httpd_register_uri_handler(server, &camera_get);
    httpd_register_uri_handler(server, &camera_save);
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

        // 已保存过 WiFi 但账号还没配完(例如上次登录失败后重启):直接连上,
        // 网页一打开就能发验证码,不用重新点第 1 步。
        device_config_t saved;
        config_load(&saved);
        if (saved.wifi_ssid[0][0]) {
            wifi_config_t sc = { 0 };
            strlcpy((char *)sc.sta.ssid, saved.wifi_ssid[0], sizeof(sc.sta.ssid));
            strlcpy((char *)sc.sta.password, saved.wifi_pass[0], sizeof(sc.sta.password));
            esp_wifi_set_config(WIFI_IF_STA, &sc);
            esp_err_t e = esp_wifi_connect();
            ESP_LOGI(TAG, "按已存 WiFi 自动连接 %s: %s",
                     saved.wifi_ssid[0], esp_err_to_name(e));
        }
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
    step(s_scr, 198, "4", "网页连WiFi+短信登录", NULL, 0);

    lv_obj_t *hint = plabel(s_scr, "全程手机完成，无需电脑；登录后自动填好账号和打印机",
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

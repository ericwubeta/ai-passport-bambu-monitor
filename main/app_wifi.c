// main/app_wifi.c —— WiFi STA 连接 + SNTP 对时(中国 NTP 源)。
#include "app_wifi.h"

#include <string.h>

#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

#include "secrets.h"

static const char *TAG = "app_wifi";

static EventGroupHandle_t s_ev;
#define EV_IP  BIT0
static bool s_time_ok;

// 多组已知网络缓存(NVS 多组或 secrets.h 单组兜底)
static char s_ssid[CFG_WIFI_MAX][33];
static char s_pass[CFG_WIFI_MAX][65];
static int  s_nets;              // 有效组数
static int  s_cur;               // 当前尝试的组下标
static int  s_fail;              // 当前组连续失败次数

// ------------------------------------------------------------ 网络选择

// 配置当前组为 STA 目标并发起连接
static void connect_current(void)
{
    if (s_nets <= 0) return;
    if (s_cur >= s_nets) s_cur = 0;
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_ssid[s_cur], sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_pass[s_cur], sizeof(wc.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    ESP_LOGI(TAG, "连接 [%d/%d] %s ...", s_cur + 1, s_nets, s_ssid[s_cur]);
}

// 阻塞扫描,在已知网络里挑信号最强的一个(周围没有已知网络则按存档顺序试)。
// 出门场景:家里 WiFi 不在场,自动选中手机热点/餐厅 WiFi,无需手动切。
static void pick_best_network(void)
{
    if (s_nets <= 1) { s_cur = 0; return; }   // 单组不用扫

    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败,按存档顺序连接");
        s_cur = 0;
        return;
    }
    uint16_t n = 16;
    wifi_ap_record_t ap[16];
    if (esp_wifi_scan_get_ap_records(&n, ap) != ESP_OK) {
        s_cur = 0;
        return;
    }
    int best = -1;
    int best_rssi = -128;
    for (int i = 0; i < s_nets; i++) {
        for (int j = 0; j < n; j++) {
            if (strncmp((char *)ap[j].ssid, s_ssid[i], sizeof(ap[j].ssid)) == 0 &&
                ap[j].rssi > best_rssi) {
                best = i;
                best_rssi = ap[j].rssi;
            }
        }
    }
    s_cur = (best >= 0) ? best : 0;
    if (best >= 0)
        ESP_LOGI(TAG, "扫描到 %d 个已知网络,选 %s(%d dBm)", s_nets, s_ssid[s_cur], best_rssi);
    else
        ESP_LOGW(TAG, "扫描未见任何已存网络(可能隐藏),按存档顺序连接");
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "拿到 IP " IPSTR, IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(s_ev, EV_IP);
    s_fail = 0;   // 连上即清零,下次断开从头算

    // 首次拿到 IP 后再启动 SNTP,避免无网空转
    static bool sntp_started;
    if (!sntp_started) {
        sntp_started = true;
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp1.aliyun.com");
        esp_netif_sntp_init(&cfg);
        ESP_LOGI(TAG, "SNTP 启动(时区 CST-8)");
    }
}

static void on_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    xEventGroupClearBits(s_ev, EV_IP);
    s_fail++;
    // 单组:无限原地重连(与旧行为一致)。
    // 多组:同一组连败 3 次换下一组 —— 走到没去过的地方,自动切换到在场的网络。
    if (s_nets > 1 && s_fail >= 3) {
        s_fail = 0;
        s_cur = (s_cur + 1) % s_nets;
        ESP_LOGW(TAG, "%s 连接失败,换下一组网络", s_ssid[(s_cur + s_nets - 1) % s_nets]);
        connect_current();
        return;
    }
    ESP_LOGW(TAG, "WiFi 断开,自动重连(%d/%d)", s_fail, s_nets > 1 ? 3 : 0);
    esp_wifi_connect();
}

void app_wifi_start(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ev = xEventGroupCreate();

    // WiFi 凭据:优先 NVS(配网页写入,支持多组),没有则回落 secrets.h(老机器兼容)。
    device_config_t cfgc;
    config_load(&cfgc);
    for (int i = 0; i < CFG_WIFI_MAX; i++) {
        if (cfgc.wifi_ssid[i][0]) {
            strlcpy(s_ssid[s_nets], cfgc.wifi_ssid[i], sizeof(s_ssid[0]));
            strlcpy(s_pass[s_nets], cfgc.wifi_pass[i], sizeof(s_pass[0]));
            s_nets++;
        }
    }
    if (s_nets == 0 && strncmp(CFG_WIFI_SSID, "YOUR_", 5) != 0) {
        strlcpy(s_ssid[0], CFG_WIFI_SSID, sizeof(s_ssid[0]));
        strlcpy(s_pass[0], CFG_WIFI_PASS, sizeof(s_pass[0]));
        s_nets = 1;
    }
    if (s_nets == 0) {
        ESP_LOGW(TAG, "NVS 与 secrets.h 都没有 WiFi 配置,跳过联网。"
                      "长按确定进菜单选'配网',或改 secrets.h 重编。");
        return;
    }

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_disconnect, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL);

    // 关闭省电模式,避免某些路由器关联超时
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 开机先扫描:家里/外面同时有已知网络时选信号强的,省去手动切
    pick_best_network();
    connect_current();   // start 只启动驱动,需手动发起关联
}

bool app_wifi_connected(void)
{
    return s_ev && (xEventGroupGetBits(s_ev) & EV_IP);
}

void app_wifi_ip_str(char *buf, int n)
{
    if (!app_wifi_connected()) { snprintf(buf, n, "0.0.0.0"); return; }
    esp_netif_t *nf = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!nf || esp_netif_get_ip_info(nf, &ip) != ESP_OK) {
        snprintf(buf, n, "0.0.0.0");
    } else {
        snprintf(buf, n, IPSTR, IP2STR(&ip.ip));
    }
}

int app_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (!app_wifi_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

bool app_wifi_time_ready(void)
{
    if (!s_time_ok && time(NULL) > 1700000000) s_time_ok = true;
    return s_time_ok;
}

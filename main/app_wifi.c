// main/app_wifi.c —— WiFi STA 连接 + SNTP 对时(中国 NTP 源)。
#include "app_wifi.h"

#include <string.h>

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

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "拿到 IP " IPSTR, IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(s_ev, EV_IP);

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
    ESP_LOGW(TAG, "WiFi 断开,自动重连");
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

    bool configured = strncmp(CFG_WIFI_SSID, "YOUR_", 5) != 0;
    if (!configured) {
        ESP_LOGW(TAG, "secrets.h 里 WiFi 还是占位符,跳过联网。"
                      "填好 CFG_WIFI_SSID/CFG_WIFI_PASS 后重启生效。");
        return;
    }

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // 调高 WiFi 日志级别,排查关联失败
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    esp_log_level_set("wifi_init", ESP_LOG_DEBUG);

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_disconnect, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, CFG_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CFG_WIFI_PASS, sizeof(wc.sta.password));
    // 关闭省电模式,避免某些路由器关联超时
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());   // start 只启动驱动,需手动发起关联
    ESP_LOGI(TAG, "WiFi 启动,连接 %s ...", CFG_WIFI_SSID);
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

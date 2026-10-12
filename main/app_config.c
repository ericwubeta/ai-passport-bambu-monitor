// main/app_config.c —— NVS 配置读写实现(见 app_config.h)。
#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

static nvs_handle_t s_handle;
static bool s_open = false;

// NVS 懒打开:nvs_flash_init 幂等(已开返回 ALREADY_OPENED),nvs_open 拿句柄。
static esp_err_t ensure_open(void) {
    if (s_open) return ESP_OK;
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES ||
        r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    // 不依赖 ESP_ERR_NVS_ALREADY_OPENED(部分 IDF 版本无此宏):
    // 直接打开命名空间,能打开即说明 NVS 已可用。
    r = nvs_open(CFG_NVS_NS, NVS_READWRITE, &s_handle);
    if (r == ESP_OK) s_open = true;
    else ESP_LOGE(TAG, "nvs_open 失败: %s", esp_err_to_name(r));
    return r;
}

bool config_load(device_config_t *cfg) {
    if (!cfg) return false;
    memset(cfg, 0, sizeof(*cfg));
    if (ensure_open() != ESP_OK) return false;

    // 每个字段用自己的缓冲区大小作 length in/out,否则 nvs_get_str 会误判缓冲区不足。
    size_t n;
    n = sizeof(cfg->wifi_ssid);   nvs_get_str(s_handle, "wifi_ssid",  cfg->wifi_ssid,  &n);
    n = sizeof(cfg->wifi_pass);   nvs_get_str(s_handle, "wifi_pass",  cfg->wifi_pass,  &n);
    n = sizeof(cfg->bambu_uid);   nvs_get_str(s_handle, "bambu_uid",  cfg->bambu_uid,  &n);
    n = sizeof(cfg->bambu_token); nvs_get_str(s_handle, "bambu_token", cfg->bambu_token, &n);
    n = sizeof(cfg->bambu_sn);    nvs_get_str(s_handle, "bambu_sn",    cfg->bambu_sn,    &n);
    return true;
}

bool config_save(const device_config_t *cfg) {
    if (!cfg || ensure_open() != ESP_OK) return false;
    bool ok = true;
    ok &= (nvs_set_str(s_handle, "wifi_ssid",  cfg->wifi_ssid)  == ESP_OK);
    ok &= (nvs_set_str(s_handle, "wifi_pass",  cfg->wifi_pass)  == ESP_OK);
    ok &= (nvs_set_str(s_handle, "bambu_uid",  cfg->bambu_uid)  == ESP_OK);
    ok &= (nvs_set_str(s_handle, "bambu_token", cfg->bambu_token) == ESP_OK);
    ok &= (nvs_set_str(s_handle, "bambu_sn",    cfg->bambu_sn)    == ESP_OK);
    ok &= (nvs_commit(s_handle) == ESP_OK);
    if (!ok) ESP_LOGE(TAG, "config_save 部分写入失败");
    return ok;
}

bool config_is_provisioned(void) {
    if (ensure_open() != ESP_OK) return false;
    uint8_t v = 0;
    if (nvs_get_u8(s_handle, "configured", &v) != ESP_OK) return false;
    return v != 0;
}

void config_mark_provisioned(void) {
    if (ensure_open() != ESP_OK) return;
    nvs_set_u8(s_handle, "configured", 1);
    nvs_commit(s_handle);
}

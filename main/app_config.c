// main/app_config.c —— NVS 配置读写实现(见 app_config.h)。
#include "app_config.h"
#include "app_secrets.h"

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
    n = sizeof(cfg->bambu_uid);   nvs_get_str(s_handle, "bambu_uid",  cfg->bambu_uid,  &n);
    n = sizeof(cfg->bambu_token); nvs_get_str(s_handle, "bambu_token", cfg->bambu_token, &n);

    // WiFi 多组:ssid0..2 / pass0..2。兼容旧单组数据:wifi_ssid 单键 → 槽 0。
    for (int i = 0; i < CFG_WIFI_MAX; i++) {
        char key[8];
        snprintf(key, sizeof(key), "ssid%d", i);
        n = sizeof(cfg->wifi_ssid[i]);
        nvs_get_str(s_handle, key, cfg->wifi_ssid[i], &n);
        snprintf(key, sizeof(key), "pass%d", i);
        n = sizeof(cfg->wifi_pass[i]);
        nvs_get_str(s_handle, key, cfg->wifi_pass[i], &n);
    }
    if (!cfg->wifi_ssid[0][0]) {   // 老数据迁移
        n = sizeof(cfg->wifi_ssid[0]);
        nvs_get_str(s_handle, "wifi_ssid", cfg->wifi_ssid[0], &n);
        n = sizeof(cfg->wifi_pass[0]);
        nvs_get_str(s_handle, "wifi_pass", cfg->wifi_pass[0], &n);
    }

    // 打印机列表:sn0..sn3 / name0..name3。兼容旧单机数据:bambu_sn 单键 → 槽 0。
    for (int i = 0; i < CFG_MACHINE_MAX; i++) {
        char key[8];
        snprintf(key, sizeof(key), "sn%d", i);
        n = sizeof(cfg->bambu_sn[i]);
        nvs_get_str(s_handle, key, cfg->bambu_sn[i], &n);
        snprintf(key, sizeof(key), "nm%d", i);
        n = sizeof(cfg->bambu_name[i]);
        nvs_get_str(s_handle, key, cfg->bambu_name[i], &n);
    }
    if (!cfg->bambu_sn[0][0]) {   // 老数据迁移
        n = sizeof(cfg->bambu_sn[0]);
        nvs_get_str(s_handle, "bambu_sn", cfg->bambu_sn[0], &n);
    }
    return true;
}

bool config_load_printers(device_config_t *cfg) {
    if (!cfg) return false;
    bool loaded = config_load(cfg);
    if (config_machine_count(cfg) == 0 && CFG_BAMBU_SERIAL[0] &&
        strcmp(CFG_BAMBU_SERIAL, "00C00AAAA000000")) {
        strlcpy(cfg->bambu_sn[0], CFG_BAMBU_SERIAL, sizeof(cfg->bambu_sn[0]));
    }
    return loaded;
}

bool config_save(const device_config_t *cfg) {
    if (!cfg || ensure_open() != ESP_OK) return false;
    bool ok = true;
    ok &= (nvs_set_str(s_handle, "bambu_uid",  cfg->bambu_uid)  == ESP_OK);
    ok &= (nvs_set_str(s_handle, "bambu_token", cfg->bambu_token) == ESP_OK);
    // WiFi 多组:ssid0..2 / pass0..2
    for (int i = 0; i < CFG_WIFI_MAX; i++) {
        char key[8];
        snprintf(key, sizeof(key), "ssid%d", i);
        ok &= (nvs_set_str(s_handle, key, cfg->wifi_ssid[i]) == ESP_OK);
        snprintf(key, sizeof(key), "pass%d", i);
        ok &= (nvs_set_str(s_handle, key, cfg->wifi_pass[i]) == ESP_OK);
    }
    // 兼容键同步到槽 0,老固件回读不丢
    ok &= (nvs_set_str(s_handle, "wifi_ssid", cfg->wifi_ssid[0]) == ESP_OK);
    ok &= (nvs_set_str(s_handle, "wifi_pass", cfg->wifi_pass[0]) == ESP_OK);
    for (int i = 0; i < CFG_MACHINE_MAX; i++) {
        char key[8];
        snprintf(key, sizeof(key), "sn%d", i);
        ok &= (nvs_set_str(s_handle, key, cfg->bambu_sn[i]) == ESP_OK);
        snprintf(key, sizeof(key), "nm%d", i);
        ok &= (nvs_set_str(s_handle, key, cfg->bambu_name[i]) == ESP_OK);
    }
    // 兼容键同步到槽 0,老固件回读不丢
    ok &= (nvs_set_str(s_handle, "bambu_sn", cfg->bambu_sn[0]) == ESP_OK);
    ok &= (nvs_commit(s_handle) == ESP_OK);
    if (!ok) ESP_LOGE(TAG, "config_save 部分写入失败");
    return ok;
}

int config_machine_count(const device_config_t *cfg) {
    if (!cfg) return 0;
    int cnt = 0;
    for (int i = 0; i < CFG_MACHINE_MAX; i++)
        if (cfg->bambu_sn[i][0]) cnt++;
    return cnt;
}

int config_wifi_count(const device_config_t *cfg) {
    if (!cfg) return 0;
    int cnt = 0;
    for (int i = 0; i < CFG_WIFI_MAX; i++)
        if (cfg->wifi_ssid[i][0]) cnt++;
    return cnt;
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

static bool printer_view_valid(const printer_view_config_t *view) {
    return view && view->version == 1 && view->slot <= 8 && view->overview <= 1 &&
           view->serial[0] && memchr(view->serial, 0, sizeof(view->serial));
}

bool config_printer_view_load(printer_view_config_t *view) {
    if (!view) return false;
    memset(view, 0, sizeof(*view));
    // A UI preference must never trigger NVS erasure or rewrite credentials.
    if (nvs_flash_init() != ESP_OK) return false;
    nvs_handle_t handle;
    if (nvs_open("printer_ui", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = sizeof(*view);
    esp_err_t err = nvs_get_blob(handle, "view", view, &size);
    nvs_close(handle);
    return err == ESP_OK && size == sizeof(*view) && printer_view_valid(view);
}

bool config_printer_view_save(const printer_view_config_t *view) {
    if (!printer_view_valid(view)) return false;
    printer_view_config_t previous;
    if (config_printer_view_load(&previous) && previous.slot == view->slot &&
        previous.overview == view->overview && !strcmp(previous.serial, view->serial)) return true;
    if (nvs_flash_init() != ESP_OK) return false;
    nvs_handle_t handle;
    if (nvs_open("printer_ui", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, "view", view, sizeof(*view));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving printer view failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Saved printer view slot=%u overview=%u", view->slot, view->overview);
    return true;
}

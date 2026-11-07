#include "app_camera_config.h"
#include <stdio.h>
#include <string.h>
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

static bool namespace_for(const char *serial, char name[16]) {
    if (!serial || !serial[0] || strlen(serial) > 33) return false;
    uint8_t hash[32];
    if (mbedtls_sha256((const uint8_t *)serial, strlen(serial), hash, 0)) return false;
    name[0] = 'c';
    for (int i = 0; i < 7; i++) snprintf(name + 1 + i * 2, 3, "%02x", hash[i]);
    return true;
}

bool camera_config_load(const char *serial, camera_config_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    char name[16], stored[34] = {0};
    if (!namespace_for(serial, name) || nvs_flash_init() != ESP_OK) return false;
    nvs_handle_t handle;
    if (nvs_open(name, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t n = sizeof(stored);
    bool ok = nvs_get_str(handle, "serial", stored, &n) == ESP_OK && !strcmp(stored, serial);
    if (ok) {
        n = sizeof(out->host); nvs_get_str(handle, "host", out->host, &n);
        n = sizeof(out->access_code); nvs_get_str(handle, "code", out->access_code, &n);
        n = sizeof(out->model); nvs_get_str(handle, "model", out->model, &n);
        n = sizeof(out->certificate_sha256);
        nvs_get_blob(handle, "cert", out->certificate_sha256, &n);
        nvs_get_u8(handle, "transport", &out->transport);
    }
    nvs_close(handle);
    return ok;
}

bool camera_config_save(const char *serial, const camera_config_t *config) {
    if (!config || config->transport > CAMERA_JPEG_STREAM ||
        !memchr(config->host, 0, sizeof(config->host)) ||
        !memchr(config->model, 0, sizeof(config->model)) ||
        !memchr(config->access_code, 0, sizeof(config->access_code))) return false;
    if (config->host[0] && strlen(config->access_code) != 8) return false;
    char name[16], stored[34] = {0};
    if (!namespace_for(serial, name) || nvs_flash_init() != ESP_OK) return false;
    nvs_handle_t handle;
    if (nvs_open(name, NVS_READWRITE, &handle) != ESP_OK) return false;
    size_t n = sizeof(stored);
    esp_err_t found = nvs_get_str(handle, "serial", stored, &n);
    bool ok = found == ESP_ERR_NVS_NOT_FOUND || (found == ESP_OK && !strcmp(stored, serial));
    if (ok) {
        ok &= nvs_set_str(handle, "serial", serial) == ESP_OK;
        ok &= nvs_set_str(handle, "host", config->host) == ESP_OK;
        ok &= nvs_set_str(handle, "code", config->access_code) == ESP_OK;
        ok &= nvs_set_str(handle, "model", config->model) == ESP_OK;
        ok &= nvs_set_u8(handle, "transport", config->transport) == ESP_OK;
        ok &= nvs_set_blob(handle, "cert", config->certificate_sha256, sizeof(config->certificate_sha256)) == ESP_OK;
        if (ok) ok = nvs_commit(handle) == ESP_OK;
    }
    nvs_close(handle);
    return ok;
}

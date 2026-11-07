// Isolated feasibility firmware. Reads existing WiFi NVS; never writes app NVS.
// Network protocol: docs/LIVE_CAMERA_FEASIBILITY.md and probe_camera_snapshot.py.
#include <string.h>
#include <stdlib.h>
#include "camera_probe_private.h"  // Generated locally; never committed.
#include "bsp_display.h"
#include "bsp_pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "esp32c3/rom/tjpgd.h"
#include "cJSON.h"

#ifndef CAMERA_PROBE_SAMPLE_COUNT
#define CAMERA_PROBE_SAMPLE_COUNT 20
#endif

static const char *TAG = "camera_probe";
static EventGroupHandle_t s_wifi;
static SemaphoreHandle_t s_flushed;
static uint8_t s_work[4096];
static uint16_t *s_stripe; // Two output rows, 960 bytes; no full image buffer.
static esp_tls_t *s_tls;
static unsigned s_tx_seq;
static int s_bin_left, s_result, s_total, s_received, s_request_seq;
static bool s_failed;
static char s_expected_md5[33];
static mbedtls_md5_context s_md5;
static unsigned s_rows;
static unsigned s_peak_frame;

static void wifi_event(void *arg, esp_event_base_t base, int32_t event, void *data) {
    if (base == WIFI_EVENT && event == WIFI_EVENT_STA_START) esp_wifi_connect();
    if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_wifi, 1);
}

static void wifi_init(void) {
    // No erase-on-error fallback: preserving the user's configuration is required.
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("aipass", NVS_READONLY, &nvs));
    wifi_config_t wifi = {0};
    char ssid[33] = {0}, pass[65] = {0};
    size_t sn = sizeof(ssid), pn = sizeof(pass);
    esp_err_t ssid_err = nvs_get_str(nvs, "ssid0", ssid, &sn);
    if (ssid_err == ESP_ERR_NVS_NOT_FOUND || (ssid_err == ESP_OK && !ssid[0])) {
        sn = sizeof(ssid);
        ESP_ERROR_CHECK(nvs_get_str(nvs, "wifi_ssid", ssid, &sn));
        ESP_ERROR_CHECK(nvs_get_str(nvs, "wifi_pass", pass, &pn));
    } else {
        ESP_ERROR_CHECK(ssid_err);
        ESP_ERROR_CHECK(nvs_get_str(nvs, "pass0", pass, &pn));
    }
    nvs_close(nvs);
    memcpy(wifi.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi.sta.password, pass, strlen(pass));
    s_wifi = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(s_wifi, 1, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    ESP_ERROR_CHECK((bits & 1) ? ESP_OK : ESP_ERR_TIMEOUT);
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static bool read_bytes(void *buffer, int length) {
    uint8_t *p = buffer;
    int64_t deadline = esp_timer_get_time() + 12000000;
    while (length > 0 && esp_timer_get_time() < deadline) {
        ssize_t n = esp_tls_conn_read(s_tls, p, length);
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
            continue;
        }
        if (n <= 0) return false;
        p += n;
        length -= n;
    }
    return length == 0;
}

static bool send_frame(uint32_t magic, const void *payload, unsigned length) {
    uint32_t header[4] = {length, magic, s_tx_seq++, 0};
    if (esp_tls_conn_write(s_tls, header, sizeof(header)) != sizeof(header)) return false;
    return esp_tls_conn_write(s_tls, payload, length) == length;
}

static cJSON *read_json(int *binary_left) {
    uint32_t header[4];
    if (!read_bytes(header, sizeof(header))) return NULL;
    uint32_t channel = header[1] & 0xffffff;
    if (channel != 0x02013f || header[0] > 1024 * 1024) return NULL;
    if (header[0] > s_peak_frame) s_peak_frame = header[0];
    char json[1024];
    bool quoted = false, escaped = false;
    int depth = 0;
    for (int i = 0; i < sizeof(json) - 1 && i < header[0]; i++) {
        char c;
        if (!read_bytes(&c, 1)) return NULL;
        json[i] = c;
        if (!i && c != '{') return NULL;
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0) {
            json[i + 1] = 0;
            *binary_left = header[0] - (i + 1);
            if (*binary_left) {
                char sep[2];
                if (*binary_left < 2 || !read_bytes(sep, 2) || memcmp(sep, "\n\n", 2)) return NULL;
                *binary_left -= 2;
            }
            return cJSON_Parse(json);
        }
    }
    return NULL;
}

static int number(cJSON *obj, const char *key, int fallback) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(value) ? value->valueint : fallback;
}

static bool next_fragment(void) {
    for (int frames = 0; frames < 8; frames++) {
        cJSON *obj = read_json(&s_bin_left);
        if (!obj) return false;
        cJSON *reply = cJSON_GetObjectItemCaseSensitive(obj, "reply");
        s_result = number(obj, "result", -1);
        bool valid = number(obj, "sequence", -1) == s_request_seq &&
                     number(obj, "cmdtype", -1) == 4 && (s_result == 0 || s_result == 1);
        int meta = number(reply, "mem_dl_param_size", 0);
        if (meta) {
            uint8_t discard[128];
            valid &= meta == s_bin_left && meta <= sizeof(discard);
            if (valid) valid = read_bytes(discard, meta);
            s_bin_left = 0;
        } else {
            int total = number(reply, "total", -1);
            valid &= total > 0 && total <= 1024 * 1024 && (!s_total || total == s_total);
            s_total = total;
            valid &= number(reply, "offset", -1) == s_received &&
                     number(reply, "size", -1) == s_bin_left;
            cJSON *md5 = cJSON_GetObjectItemCaseSensitive(reply, "file_md5");
            if (cJSON_IsString(md5) && strlen(md5->valuestring) == 32)
                memcpy(s_expected_md5, md5->valuestring, 33);
        }
        cJSON_Delete(obj);
        if (!valid) return false;
        if (!meta) return true;
    }
    return false;
}

static UINT jpeg_input(JDEC *decoder, BYTE *destination, UINT requested) {
    uint8_t discard[512];
    unsigned got = 0;
    while (got < requested && !s_failed) {
        if (!s_bin_left) {
            if (s_result == 0) break;
            if (!next_fragment()) { s_failed = true; break; }
        }
        unsigned length = requested - got;
        if (length > s_bin_left) length = s_bin_left;
        if (length > sizeof(discard)) length = sizeof(discard);
        uint8_t *target = destination ? destination + got : discard;
        if (!read_bytes(target, length)) { s_failed = true; break; }
        mbedtls_md5_update(&s_md5, target, length);
        s_bin_left -= length;
        s_received += length;
        got += length;
    }
    return got;
}

static bool flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *context) {
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(s_flushed, &wake);
    return wake == pdTRUE;
}

static bool draw_stripe(int y, int rows) {
    esp_err_t err = esp_lcd_panel_draw_bitmap(bsp_display_panel(), 0, y, 240, y + rows, s_stripe);
    return err == ESP_OK && xSemaphoreTake(s_flushed, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static UINT jpeg_output(JDEC *decoder, void *pixels, JRECT *rect) {
    uint8_t *rgb = pixels;
    int width = rect->right - rect->left + 1;
    int height = rect->bottom - rect->top + 1;
    if (rect->right >= 240 || rect->bottom >= 135 || height > 2) return 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint16_t pixel = ((rgb[0] & 0xf8) << 8) | ((rgb[1] & 0xfc) << 3) | (rgb[2] >> 3);
            s_stripe[y * 240 + rect->left + x] = __builtin_bswap16(pixel);
            rgb += 3;
        }
    }
    if (rect->right == 239) {
        if (!draw_stripe(92 + rect->top, height)) return 0;
        s_rows += height;
    }
    return 1;
}

static bool snapshot(void) {
    s_tls = esp_tls_init();
    if (!s_tls) return false;
    esp_tls_cfg_t tls_cfg = {.timeout_ms = 10000, .common_name = CAMERA_SERIAL};
    bool ok = false;
    do {
        if (esp_tls_conn_new_sync(CAMERA_HOST, strlen(CAMERA_HOST), 6000, &tls_cfg, s_tls) != 1) break;
        mbedtls_ssl_context *ssl = esp_tls_get_ssl_context(s_tls);
        const mbedtls_x509_crt *cert = mbedtls_ssl_get_peer_cert(ssl);
        uint8_t digest[32];
        if (!cert || mbedtls_sha256(cert->raw.p, cert->raw.len, digest, 0) ||
            memcmp(digest, CAMERA_CERT_SHA256, sizeof(digest))) break;
        s_tx_seq = 1;
        uint8_t auth[16] = {'b','b','l','p',0,0,0,0};
        memcpy(auth + 8, CAMERA_ACCESS_CODE, 8);
        if (!send_frame(0x0101013f, auth, sizeof(auth))) break;
        uint32_t ack[5];
        if (!read_bytes(ack, sizeof(ack)) || ack[0] != 4 || (ack[1] & 0xffffff) != 0x01013f || ack[4]) break;
        const char setup[] = "{\"sequence\":0,\"mtype\":12291,\"req\":{\"t_av\":0,\"mtype\":12289,\"peer_t\":3,\"pid\":\"\",\"ver\":\"\"}}";
        if (!send_frame(0x0102013f, setup, strlen(setup))) break;
        int left = 0;
        cJSON *obj = read_json(&left);
        bool ready = obj && !left && number(obj, "result", -1) == 0 && number(obj, "mtype", -1) == 12291;
        cJSON_Delete(obj);
        if (!ready) break;
        s_request_seq = s_tx_seq;
        char param[160], request[384];
        int pn = snprintf(param, sizeof(param), "{\"command\":\"get_project_file\",\"file_rel_path\":\"\",\"peer_host\":\"studio\",\"sequence_id\":%d,\"version\":1}", s_request_seq);
        int rn = snprintf(request, sizeof(request), "{\"mtype\":12289,\"cmdtype\":4,\"sequence\":%d,\"req\":{\"mem_dl_param_size\":%d,\"offset\":0,\"path\":\"mem:/26\"}}\n\n%s", s_request_seq, pn, param);
        if (pn < 0 || pn >= sizeof(param) || rn < 0 || rn >= sizeof(request) || !send_frame(0x0102013f, request, rn)) break;
        s_result = 1;
        s_bin_left = s_total = s_received = s_rows = s_peak_frame = 0;
        s_failed = false;
        s_expected_md5[0] = 0;
        mbedtls_md5_init(&s_md5);
        mbedtls_md5_starts(&s_md5);
        JDEC decoder = {0};
        JRESULT prepared = jd_prepare(&decoder, jpeg_input, s_work, sizeof(s_work), NULL);
        JRESULT decoded = JDR_PAR;
        if (prepared == JDR_OK && decoder.width == 1920 && decoder.height == 1080)
            decoded = jd_decomp(&decoder, jpeg_output, 3);
        // Drain the tail/EOI already unneeded by the decoder, including final RPC.
        uint8_t tail[512];
        while (!s_failed && (s_result != 0 || s_bin_left)) {
            if (!jpeg_input(&decoder, tail, sizeof(tail))) break;
        }
        uint8_t md5[16];
        char hex[33];
        mbedtls_md5_finish(&s_md5, md5);
        mbedtls_md5_free(&s_md5);
        for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", md5[i]);
        bool integrity = !s_failed && s_result == 0 && s_received == s_total &&
                         !strcmp(hex, s_expected_md5);
        ok = prepared == JDR_OK && decoded == JDR_OK && integrity && s_rows == 135;
        ESP_LOGI(TAG, "image prepare=%d decode=%d bytes=%d rows=%u md5_ok=%d frame_payload=%u heap=%u largest=%u min_heap=%u",
                 prepared, decoded, s_received, s_rows, integrity, s_peak_frame,
                 (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned)esp_get_minimum_free_heap_size());
    } while (0);
    esp_tls_conn_destroy(s_tls);
    s_tls = NULL;
    return ok;
}

void app_main(void) {
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "isolated snapshot probe; no PSRAM, no LVGL/MQTT; NVS read only");
    wifi_init();
    ESP_ERROR_CHECK(bsp_display_init());
    s_flushed = xSemaphoreCreateBinary();
    s_stripe = heap_caps_calloc(240 * 2, sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(s_flushed && s_stripe ? ESP_OK : ESP_ERR_NO_MEM);
    esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = flush_done};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(bsp_display_io(), &callbacks, NULL));
    for (int y = 0; y < 320; y += 2) ESP_ERROR_CHECK(draw_stripe(y, 2) ? ESP_OK : ESP_FAIL);
    bsp_display_backlight(80);
    unsigned passed = 0;
    for (unsigned i = 0; i < CAMERA_PROBE_SAMPLE_COUNT; i++) {
        int64_t start = esp_timer_get_time();
        bool ok = snapshot();
        if (ok) passed++;
        ESP_LOGI(TAG, "sample=%u ok=%d total_passed=%u elapsed_ms=%lld heap_after_close=%u stack_watermark=%u",
                 i, ok, passed, (esp_timer_get_time() - start) / 1000,
                 (unsigned)esp_get_free_heap_size(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
        if (!ok) break;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "probe_finished passed=%u", passed);
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}

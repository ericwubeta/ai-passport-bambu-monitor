// Transport/decode run outside LVGL. Buffered targets retain only the fitted
// RGB565 preview; neither a complete JPEG nor source-resolution RGB is stored.
#include "app_camera.h"
#include "app_camera_config.h"
#include "app_mqtt.h"
#include "app_wifi.h"
#include "bsp_display.h"
#include "camera_geometry.h"
#include "camera_frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md5.h"
#include "mbedtls/oid.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "rom/tjpgd.h"

#define IMAGE_LIMIT (2 * 1024 * 1024)
#define JSON_LIMIT 1024
#ifndef CAMERA_SUCCESS_DELAY_MS
#ifdef CONFIG_CAMERA_REFRESH_DELAY_MS
#define CAMERA_SUCCESS_DELAY_MS CONFIG_CAMERA_REFRESH_DELAY_MS
#else
#define CAMERA_SUCCESS_DELAY_MS 2000
#endif
#endif
#ifndef CAMERA_REUSE_SESSION
#ifdef CONFIG_CAMERA_REUSE_SESSION
#define CAMERA_REUSE_SESSION 1
#else
#define CAMERA_REUSE_SESSION 0
#endif
#endif
static const char *TAG = "camera";
static portMUX_TYPE s_guard = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static bool s_starting, s_requested, s_refresh;
static uint32_t s_generation;
static int s_machine, s_x, s_y, s_width, s_height;
static camera_state_t s_state;

typedef struct {
    uint32_t token, sequence, request;
    char serial[34];
    camera_config_t config;
    camera_geometry_t geometry;
    int x, y, vw, vh, left, result, total, received, band_rows;
    bool failed, drew, identity_failed, certificate_seen, jpeg_stream, tiled, buffered;
    camera_status_t error;
    uint8_t candidate_pin[32], tail[2];
    char md5_expected[33], json[JSON_LIMIT];
    esp_tls_t *tls;
    mbedtls_md5_context md5;
    uint8_t *work;
    uint16_t *stripe;
    camera_frame_t frame;
    int64_t io_us, draw_us;
    unsigned reads, draws;
    char image_md5[33], previous_md5[33];
} camera_reader_t;
static camera_reader_t *s_verifying;

static bool current(uint32_t token) {
    portENTER_CRITICAL(&s_guard);
    bool valid = s_requested && s_generation == token;
    portEXIT_CRITICAL(&s_guard);
    return valid;
}

static void status(camera_reader_t *r, camera_status_t value, bool success) {
    portENTER_CRITICAL(&s_guard);
    if (s_requested && s_generation == r->token) {
        s_state.status = value;
        if (success) {
            s_state.has_image = true;
            s_state.updated_us = esp_timer_get_time();
            camera_rate_record(&s_state.rate, s_state.updated_us);
            s_state.source_w = r->geometry.source_w;
            s_state.source_h = r->geometry.source_h;
            s_state.frames++;
        } else if (r->drew) {
            s_state.has_image = false;
        }
    }
    portEXIT_CRITICAL(&s_guard);
}

static int verify_certificate(void *arg, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    camera_reader_t *r = arg;
    if (depth != 0) { *flags = 0; return 0; }
    r->certificate_seen = true;
    bool identity = false;
    for (const mbedtls_x509_name *name = &crt->subject; name; name = name->next) {
        if (name->oid.len == sizeof(MBEDTLS_OID_AT_CN) - 1 &&
            !memcmp(name->oid.p, MBEDTLS_OID_AT_CN, name->oid.len) &&
            name->val.len == strlen(r->serial) && !memcmp(name->val.p, r->serial, name->val.len)) identity = true;
    }
    if (mbedtls_sha256(crt->raw.p, crt->raw.len, r->candidate_pin, 0)) identity = false;
    uint8_t zero[32] = {0};
    if (memcmp(r->config.certificate_sha256, zero, sizeof(zero)) &&
        memcmp(r->candidate_pin, r->config.certificate_sha256, sizeof(zero))) identity = false;
    r->identity_failed = !identity;
    *flags = identity ? 0 : MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    return 0;
}

static esp_err_t attach_verifier(void *config) {
    // Authenticate the expected printer's pinned leaf, not the public web PKI.
    // On first pairing the CN must match the configured SN; persist that leaf.
    // mbedTLS requires a CA chain in VERIFY_REQUIRED even when our callback
    // pins a leaf. OPTIONAL permits that handshake; connect_camera enforces
    // the callback result before sending any application authentication.
    mbedtls_ssl_conf_authmode(config, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(config, verify_certificate, s_verifying);
    if (mbedtls_ssl_conf_max_frag_len(config, MBEDTLS_SSL_MAX_FRAG_LEN_4096)) return ESP_FAIL;
    return ESP_OK;
}

static bool connect_camera(camera_reader_t *r) {
    r->tls = esp_tls_init();
    if (!r->tls) { r->error = CAMERA_MEMORY_ERROR; return false; }
    s_verifying = r;
    esp_tls_cfg_t cfg = {.timeout_ms = 2000, .common_name = r->serial, .crt_bundle_attach = attach_verifier};
    if (esp_tls_conn_new_sync(r->config.host, strlen(r->config.host), 6000, &cfg, r->tls) != 1) {
        if (r->identity_failed) r->error = CAMERA_IDENTITY_ERROR;
        return false;
    }
    if (!r->certificate_seen || r->identity_failed) {
        r->error = CAMERA_IDENTITY_ERROR;
        return false;
    }
    ESP_LOGI(TAG, "TLS receive_limit=%u", (unsigned)mbedtls_ssl_get_max_in_record_payload(esp_tls_get_ssl_context(r->tls)));
    if (!current(r->token)) return false;
    if (memcmp(r->candidate_pin, r->config.certificate_sha256, 32)) {
        camera_config_t pinned = r->config;
        memcpy(pinned.certificate_sha256, r->candidate_pin, 32);
        if (!camera_config_save(r->serial, &pinned)) return false;
        r->config = pinned;
    }
    return true;
}

static void close_camera(camera_reader_t *r) {
    if (r->tls) esp_tls_conn_destroy(r->tls);
    r->tls = NULL;
    if (s_verifying == r) s_verifying = NULL;
}

static bool read_bytes_inner(camera_reader_t *r, void *destination, int length) {
    uint8_t *p = destination;
    int64_t deadline = esp_timer_get_time() + 7000000;
    while (length > 0 && current(r->token) && esp_timer_get_time() < deadline) {
        int n = esp_tls_conn_read(r->tls, p, length);
        // A reused TLS session can renegotiate. Never pass data from a failed
        // identity callback to the JPEG decoder, even in OPTIONAL TLS mode.
        if (r->identity_failed) { r->error = CAMERA_IDENTITY_ERROR; return false; }
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_TIMEOUT) {
            vTaskDelay(1); continue;
        }
        if (n <= 0) {
            if (n == MBEDTLS_ERR_SSL_ALLOC_FAILED) r->error = CAMERA_MEMORY_ERROR;
            return false;
        }
        p += n; length -= n;
    }
    return length == 0;
}

static bool read_bytes(camera_reader_t *r, void *destination, int length) {
    int64_t start = esp_timer_get_time();
    bool ok = read_bytes_inner(r, destination, length);
    r->io_us += esp_timer_get_time() - start;
    r->reads++;
    return ok;
}

static bool write_bytes(camera_reader_t *r, const void *source, int length) {
    const uint8_t *p = source;
    int64_t deadline = esp_timer_get_time() + 7000000;
    while (length > 0 && current(r->token) && esp_timer_get_time() < deadline) {
        int n = esp_tls_conn_write(r->tls, p, length);
        if (r->identity_failed) { r->error = CAMERA_IDENTITY_ERROR; return false; }
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) { vTaskDelay(1); continue; }
        if (n <= 0) return false;
        p += n; length -= n;
    }
    return length == 0;
}

static bool send_frame(camera_reader_t *r, uint32_t magic, const void *payload, int size) {
    uint32_t head[4] = {size, magic, r->sequence++, 0};
    return write_bytes(r, head, sizeof(head)) && write_bytes(r, payload, size);
}

static cJSON *read_json(camera_reader_t *r, int *left) {
    uint32_t head[4];
    if (!read_bytes(r, head, sizeof(head)) || (head[1] & 0xffffff) != 0x02013f || head[0] > IMAGE_LIMIT) return NULL;
    bool quoted = false, escaped = false;
    int depth = 0;
    for (int i = 0; i < sizeof(r->json) - 1 && i < head[0]; i++) {
        char c;
        if (!read_bytes(r, &c, 1)) return NULL;
        r->json[i] = c;
        if (!i && c != '{') return NULL;
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0) {
            r->json[i + 1] = 0;
            *left = head[0] - i - 1;
            if (*left) {
                char sep[2];
                if (*left < 2 || !read_bytes(r, sep, 2) || memcmp(sep, "\n\n", 2)) return NULL;
                *left -= 2;
            }
            return cJSON_Parse(r->json);
        }
    }
    return NULL;
}

static int number(cJSON *obj, const char *key, int fallback) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(value) ? value->valueint : fallback;
}

static bool start_snapshot_session(camera_reader_t *r) {
    r->sequence = 1;
    uint8_t auth[16] = {'b','b','l','p',0,0,0,0};
    memcpy(auth + 8, r->config.access_code, 8);
    if (!send_frame(r, 0x0101013f, auth, sizeof(auth))) return false;
    uint32_t ack[5];
    if (!read_bytes(r, ack, sizeof(ack))) return false;
    if (ack[0] != 4 || (ack[1] & 0xffffff) != 0x01013f || ack[4]) {
        r->error = CAMERA_UNSUPPORTED; return false;
    }
    const char setup[] = "{\"sequence\":0,\"mtype\":12291,\"req\":{\"t_av\":0,\"mtype\":12289,\"peer_t\":3,\"pid\":\"\",\"ver\":\"\"}}";
    if (!send_frame(r, 0x0102013f, setup, strlen(setup))) return false;
    int left = 0;
    cJSON *obj = read_json(r, &left);
    bool ready = obj && !left && number(obj, "result", -1) == 0 && number(obj, "mtype", -1) == 12291;
    cJSON_Delete(obj);
    return ready;
}

static bool request_snapshot(camera_reader_t *r) {
    r->request = r->sequence;
    char param[160], request[384];
    int pn = snprintf(param, sizeof(param), "{\"command\":\"get_project_file\",\"file_rel_path\":\"\",\"peer_host\":\"studio\",\"sequence_id\":%u,\"version\":1}", (unsigned)r->request);
    int rn = snprintf(request, sizeof(request), "{\"mtype\":12289,\"cmdtype\":4,\"sequence\":%u,\"req\":{\"mem_dl_param_size\":%d,\"offset\":0,\"path\":\"mem:/26\"}}\n\n%s", (unsigned)r->request, pn, param);
    return pn > 0 && pn < sizeof(param) && rn > 0 && rn < sizeof(request) && send_frame(r, 0x0102013f, request, rn);
}

static bool next_fragment(camera_reader_t *r) {
    for (int frames = 0; frames < 8; frames++) {
        cJSON *obj = read_json(r, &r->left);
        if (!obj) return false;
        cJSON *reply = cJSON_GetObjectItemCaseSensitive(obj, "reply");
        r->result = number(obj, "result", -1);
        bool valid = number(obj, "sequence", -1) == r->request && number(obj, "cmdtype", -1) == 4;
        if (r->result != 0 && r->result != 1) { valid = false; r->error = CAMERA_UNSUPPORTED; }
        int meta = number(reply, "mem_dl_param_size", 0);
        if (meta) {
            uint8_t discard[128];
            valid &= meta == r->left && meta <= sizeof(discard);
            if (valid) valid = read_bytes(r, discard, meta);
            r->left = 0;
        } else {
            int total = number(reply, "total", -1);
            valid &= total > 0 && total <= IMAGE_LIMIT && (!r->total || total == r->total);
            r->total = total;
            valid &= number(reply, "offset", -1) == r->received && number(reply, "size", -1) == r->left;
            cJSON *md5 = cJSON_GetObjectItemCaseSensitive(reply, "file_md5");
            if (cJSON_IsString(md5) && strlen(md5->valuestring) == 32) memcpy(r->md5_expected, md5->valuestring, 33);
        }
        cJSON_Delete(obj);
        if (!valid) return false;
        if (!meta) return true;
    }
    return false;
}

static bool begin_jpeg_stream(camera_reader_t *r) {
    uint32_t auth[20] = {0x40, 0x3000, 0, 0};
    memcpy((uint8_t *)auth + 16, "bblp", 4);
    memcpy((uint8_t *)auth + 48, r->config.access_code, 8);
    if (!write_bytes(r, auth, sizeof(auth))) return false;
    // Older cameras first replay a buffered frame. Discard it, then read a new
    // one, rather than reporting a potentially old still as a fresh snapshot.
    for (int frame = 0; frame < 2; frame++) {
        uint32_t header[4];
        if (!read_bytes(r, header, sizeof(header))) return false;
        if (header[0] < 4 || header[0] > IMAGE_LIMIT) { r->error = CAMERA_UNSUPPORTED; return false; }
        if (frame == 1) { r->left = r->total = header[0]; r->result = 0; return true; }
        uint8_t discard[512];
        unsigned remaining = header[0];
        while (remaining) {
            unsigned n = remaining < sizeof(discard) ? remaining : sizeof(discard);
            if (!read_bytes(r, discard, n)) return false;
            remaining -= n;
        }
    }
    return false;
}

static UINT jpeg_input(JDEC *decoder, BYTE *destination, UINT requested) {
    camera_reader_t *r = decoder->device;
    uint8_t discard[512];
    unsigned got = 0;
    while (got < requested && !r->failed && current(r->token)) {
        if (!r->left) {
            if (r->result == 0) break;
            if (!next_fragment(r)) { r->failed = true; break; }
        }
        unsigned n = requested - got;
        if (n > r->left) n = r->left;
        if (n > sizeof(discard)) n = sizeof(discard);
        uint8_t *target = destination ? destination + got : discard;
        if (!read_bytes(r, target, n)) { r->failed = true; break; }
        if (r->received == 0 && n >= 3)
            ESP_LOGI(TAG, "input jpeg_signature=%d bytes=%u", target[0]==0xff && target[1]==0xd8 && target[2]==0xff, n);
        if (n >= 2) { r->tail[0] = target[n-2]; r->tail[1] = target[n-1]; }
        else if (n == 1) { r->tail[0] = r->tail[1]; r->tail[1] = target[0]; }
        mbedtls_md5_update(&r->md5, target, n);
        r->received += n; r->left -= n; got += n;
    }
    return got;
}

static bool draw_stripe(camera_reader_t *r, int x, int y, int width, int height) {
    int64_t start = esp_timer_get_time();
    bool ok = bsp_lvgl_stream_draw(r->token, x, y, width, height, r->stripe);
    r->draw_us += esp_timer_get_time() - start;
    r->draws++;
    return ok;
}

static void *allocate_preview(size_t bytes) {
    // ESP32 prefers unused instruction RAM for aligned 32-bit-only allocations.
    return heap_caps_malloc(bytes, MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL);
}

static bool present_frame(camera_reader_t *r) {
    if (!bsp_lvgl_lock(150)) return false;
    bool ok = current(r->token) && bsp_lvgl_stream_set(r->y, r->vh, r->token);
    // Hold the recursive display lock only for this short commit, never during
    // network I/O or JPEG decoding. LVGL cannot interleave unrelated flushes.
    for (int y = 0; ok && y < r->vh; y += CAMERA_FRAME_ROWS) {
        int rows = r->vh - y < CAMERA_FRAME_ROWS ? r->vh - y : CAMERA_FRAME_ROWS;
        camera_frame_copy_rows(&r->frame, y, rows, (uint32_t *)r->stripe);
        r->drew = true;
        ok = current(r->token) && draw_stripe(r, r->x, r->y + y, r->vw, rows);
    }
    bsp_lvgl_unlock();
    return ok;
}

static UINT jpeg_output(JDEC *decoder, void *bitmap, JRECT *rect) {
    camera_reader_t *r = decoder->device;
    const camera_geometry_t *g = &r->geometry;
    const uint8_t *rgb = bitmap;
    int rw = rect->right - rect->left + 1;
    int y0 = camera_geometry_edge(rect->top, g->height, g->decoded_h);
    int y1 = camera_geometry_edge(rect->bottom + 1, g->height, g->decoded_h);
    int x0 = camera_geometry_edge(rect->left, g->width, g->decoded_w);
    int x1 = camera_geometry_edge(rect->right + 1, g->width, g->decoded_w);
    if ((!r->buffered && y1 - y0 > r->band_rows) || !current(r->token)) return 0;
    for (int dy = y0; dy < y1; dy++) {
        int sy = dy * g->decoded_h / g->height - rect->top;
        for (int dx = x0; dx < x1; dx++) {
            int sx = dx * g->decoded_w / g->width - rect->left;
            const uint8_t *p = rgb + (sy * rw + sx) * 3;
            uint16_t pixel = ((p[0] & 0xf8) << 8) | ((p[1] & 0xfc) << 3) | (p[2] >> 3);
            if (r->buffered) {
                camera_frame_pixel(&r->frame, g->x + dx, g->y + dy, __builtin_bswap16(pixel));
            } else {
                int offset = r->tiled ? (dy-y0)*(x1-x0) + dx-x0 : (dy-y0)*g->width+dx;
                r->stripe[offset] = __builtin_bswap16(pixel);
            }
        }
    }
    if (r->buffered) return 1;
    if (r->tiled && x1 > x0 && y1 > y0) {
        if (!draw_stripe(r, r->x+g->x+x0, r->y+g->y+y0, x1-x0, y1-y0)) return 0;
    } else if (!r->tiled && rect->right >= g->decoded_w - 1 && y1 > y0) {
        if (!draw_stripe(r, r->x + g->x, r->y + g->y + y0, g->width, y1-y0)) return 0;
    }
    return 1;
}

static bool decode(camera_reader_t *r) {
    camera_geometry_t previous_geometry = r->geometry;
    camera_state_t previous_state;
    app_camera_snapshot(&previous_state);
    r->buffered = false;
#ifdef CONFIG_CAMERA_BUFFERED_PREVIEW
    r->buffered = true;
#endif
    JDEC decoder = {0};
    mbedtls_md5_init(&r->md5); mbedtls_md5_starts(&r->md5);
    JRESULT prepared = jd_prepare(&decoder, jpeg_input, r->work, 4096, r);
    ESP_LOGI(TAG, "decode prepare=%d input_failed=%d bytes=%d total=%d jpeg=%d viewport=%dx%d source=%ux%u",
             prepared, r->failed, r->received, r->total, r->jpeg_stream, r->vw, r->vh, decoder.width, decoder.height);
    bool ok = prepared == JDR_OK && camera_geometry_fit(decoder.width, decoder.height, r->vw, r->vh, &r->geometry);
    if (!ok && r->error != CAMERA_MEMORY_ERROR)
        r->error = prepared == JDR_MEM1 || prepared == JDR_MEM2 ? CAMERA_MEMORY_ERROR :
                   r->failed || prepared == JDR_INP ? CAMERA_RETRYING : CAMERA_UNSUPPORTED;
    if (ok) {
        ok = current(r->token);
    }
    if (ok) {
        int mcu_rows = (decoder.msy * 8) >> r->geometry.scale;
        if (mcu_rows < 1) mcu_rows = 1;
        r->band_rows = (mcu_rows * r->geometry.height + r->geometry.decoded_h - 1) / r->geometry.decoded_h;
        r->tiled = false;
        if (r->buffered) {
            bool allocating = r->frame.width != r->vw || r->frame.height != r->vh;
            size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            ok = camera_frame_alloc(&r->frame, r->vw, r->vh, allocate_preview);
            if (allocating && ok) ESP_LOGI(TAG, "preview bytes=%u dram_used=%u heap32=%u",
                (unsigned)(r->frame.stride_words * 4 * r->vh),
                (unsigned)(before - heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_32BIT));
            if (ok) {
                camera_frame_clear(&r->frame);
                r->stripe = heap_caps_calloc(r->vw * CAMERA_FRAME_ROWS, sizeof(uint16_t), MALLOC_CAP_DMA);
                // Keep byte-accessible RAM available for MQTT/TLS reconnects.
                // TLS has active record buffers at this point; reserve another
                // 16 KiB of byte-accessible RAM for concurrent network work.
                ok = r->stripe && heap_caps_get_free_size(MALLOC_CAP_8BIT) >= 16384;
            }
            if (!ok) { camera_frame_free(&r->frame); r->error = CAMERA_MEMORY_ERROR; }
        } else r->stripe = heap_caps_calloc(r->vw * r->band_rows, sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!r->buffered && !r->stripe) {
            // A small source image can need a taller MCU band. Under pressure,
            // display each resampled MCU instead of retaining a full-width band.
            // No upscaling means at most 16x16 output pixels per MCU.
            r->stripe = heap_caps_calloc(256, sizeof(uint16_t), MALLOC_CAP_DMA);
            r->tiled = true;
        }
        if (!r->stripe) { r->error = CAMERA_MEMORY_ERROR; ok = false; }
    }
    if (ok && !r->buffered) {
        if (bsp_lvgl_lock(150)) {
            ok = current(r->token) && bsp_lvgl_stream_set(r->y, r->vh, r->token);
            bsp_lvgl_unlock();
        } else ok = false;
    }
    if (ok && !r->buffered) {
        r->drew = true;
        // Retain the previous frame while the next arrives. Clear only when
        // letterboxes change or no verified frame exists, avoiding black flashes.
        if (!previous_state.has_image || previous_geometry.width != r->geometry.width ||
            previous_geometry.height != r->geometry.height) {
            int clear_rows = r->tiled ? 1 : r->band_rows;
            for (int y = 0; y < r->vh && ok; y += clear_rows) {
                int h = r->vh-y < clear_rows ? r->vh-y : clear_rows;
                ok = draw_stripe(r, r->x, r->y+y, r->vw, h);
            }
        }
    }
    if (ok) {
        JRESULT result = jd_decomp(&decoder, jpeg_output, r->geometry.scale);
        ESP_LOGI(TAG, "decode result=%d input_failed=%d", result, r->failed);
        ok = result == JDR_OK;
    }
    if (ok) {
        uint8_t tail[512];
        while (!r->failed && current(r->token) && (r->result != 0 || r->left)) {
            if (!jpeg_input(&decoder, tail, sizeof(tail))) break;
        }
        uint8_t md5[16]; char hex[33];
        mbedtls_md5_finish(&r->md5, md5);
        for (int i = 0; i < 16; i++) snprintf(hex+2*i, 3, "%02x", md5[i]);
        ok = !r->failed && current(r->token) && r->received == r->total && r->result == 0;
        ok &= r->jpeg_stream ? r->tail[0] == 0xff && r->tail[1] == 0xd9 :
                              r->md5_expected[0] && !strcmp(hex, r->md5_expected);
        if (ok) memcpy(r->image_md5, hex, sizeof(hex));
    }
    mbedtls_md5_free(&r->md5);
    if (ok && r->buffered) {
        int64_t start = esp_timer_get_time();
        ok = present_frame(r);
        r->draw_us = esp_timer_get_time() - start;
    }
    free(r->stripe); r->stripe = NULL;
    if (!ok && r->drew && bsp_lvgl_lock(150)) { bsp_lvgl_stream_end(r->token); bsp_lvgl_unlock(); }
    return ok;
}

static bool fetch(camera_reader_t *r, bool jpeg_stream) {
    if (jpeg_stream) close_camera(r); // Unverified stream models retain their existing reconnect path.
    r->jpeg_stream = jpeg_stream;
    r->left = r->total = r->received = 0; r->result = 1;
    r->failed = r->drew = false; r->md5_expected[0] = r->image_md5[0] = 0;
    r->io_us = r->draw_us = 0; r->reads = r->draws = 0;
    r->error = CAMERA_RETRYING;
    int64_t start = esp_timer_get_time();
    bool reused = r->tls != NULL;
    bool ok = reused;
    if (!reused) {
        r->identity_failed = r->certificate_seen = false;
        ok = connect_camera(r);
    }
    int64_t connected = esp_timer_get_time();
    if (ok && !reused && !jpeg_stream) ok = start_snapshot_session(r);
    if (ok) ok = jpeg_stream ? begin_jpeg_stream(r) : request_snapshot(r);
    int64_t ready = esp_timer_get_time(), prior_io = r->io_us;
    if (ok) ok = decode(r);
    int64_t done = esp_timer_get_time();
    bool changed = ok && r->image_md5[0] && strcmp(r->image_md5, r->previous_md5);
    if (ok) memcpy(r->previous_md5, r->image_md5, sizeof(r->previous_md5));
    ESP_LOGI(TAG, "perf reuse=%d tls_ms=%lld setup_ms=%lld body_io_ms=%lld cpu_ms=%lld draw_ms=%lld bytes=%d reads=%u draws=%u changed=%d",
        reused, (connected-start)/1000, (ready-connected)/1000, (r->io_us-prior_io)/1000,
        (done-ready-(r->io_us-prior_io)-r->draw_us)/1000, r->draw_us/1000, r->received, r->reads, r->draws, changed);
    if (!ok || jpeg_stream || !CAMERA_REUSE_SESSION) close_camera(r);
    if (!ok) camera_frame_free(&r->frame);
    return ok;
}

static void worker(void *unused) {
    // ESP32 can schedule this task on the other core before xTaskCreate returns.
    // Wait until the owner has published its handle, including the OOM path.
    for (;;) {
        portENTER_CRITICAL(&s_guard); bool starting = s_starting; portEXIT_CRITICAL(&s_guard);
        if (!starting) break;
        vTaskDelay(1);
    }
    camera_reader_t *r = calloc(1, sizeof(*r));
    if (r) r->work = malloc(4096);
    if (!r || !r->work) {
        portENTER_CRITICAL(&s_guard); s_state.status = CAMERA_MEMORY_ERROR; portEXIT_CRITICAL(&s_guard);
    }
    while (r && r->work) {
        // A page generation or selected printer change always owns a new TLS session.
        close_camera(r);
        camera_frame_free(&r->frame);
        portENTER_CRITICAL(&s_guard);
        bool requested = s_requested;
        r->token = s_generation; int machine = s_machine;
        r->x=s_x; r->y=s_y; r->vw=s_width; r->vh=s_height;
        portEXIT_CRITICAL(&s_guard);
        if (!requested) {
            portENTER_CRITICAL(&s_guard);
            if (!s_requested) { s_task = NULL; }
            else requested = true;
            portEXIT_CRITICAL(&s_guard);
            if (!requested) break;
            continue;
        }
        strlcpy(r->serial, app_mqtt_sn(machine), sizeof(r->serial));
        camera_config_load(r->serial, &r->config);
        if (!r->config.host[0] || strlen(r->config.access_code) != 8) {
            status(r, CAMERA_UNCONFIGURED, false);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        portENTER_CRITICAL(&s_guard);
        if (s_generation == r->token) strlcpy(s_state.model, r->config.model[0] ? r->config.model :
            (app_mqtt_name(machine) ? app_mqtt_name(machine) : "Printer"), sizeof(s_state.model));
        portEXIT_CRITICAL(&s_guard);
        bool jpeg = r->config.transport == CAMERA_JPEG_STREAM;
        while (current(r->token)) {
            bool ok = false;
            // This attempt may skip fetch entirely while WiFi is offline.
            // Only drawing in this attempt can invalidate the last good image.
            r->drew = false;
            r->error = CAMERA_RETRYING;
            int64_t start = esp_timer_get_time();
            if (app_wifi_connected()) {
                ok = fetch(r, jpeg);
                if (!ok && !jpeg && r->config.transport == CAMERA_AUTO && !r->drew &&
                    r->error != CAMERA_IDENTITY_ERROR && r->error != CAMERA_MEMORY_ERROR && current(r->token)) {
                    ok = fetch(r, true);
                    if (ok) jpeg = true;
                }
            } else {
                close_camera(r);
                camera_frame_free(&r->frame);
            }
            status(r, ok ? CAMERA_READY : r->error, ok);
            ESP_LOGI(TAG, "frame ok=%d status=%d source=%dx%d fitted=%dx%d ms=%lld heap=%u largest=%u stack=%u",
                ok, ok ? CAMERA_READY : r->error, r->geometry.source_w, r->geometry.source_h,
                r->geometry.width, r->geometry.height, (esp_timer_get_time()-start)/1000,
                (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
            unsigned delay_ms = ok ? CAMERA_SUCCESS_DELAY_MS : 5000;
            for (unsigned waited=0; waited<delay_ms && current(r->token); waited+=100) {
                portENTER_CRITICAL(&s_guard); bool refresh = s_refresh; s_refresh = false; portEXIT_CRITICAL(&s_guard);
                if (refresh) break;
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }
    if (r) { close_camera(r); camera_frame_free(&r->frame); free(r->work); free(r); }
    portENTER_CRITICAL(&s_guard);
    if (s_task == xTaskGetCurrentTaskHandle()) { s_task = NULL; s_starting = false; }
    portEXIT_CRITICAL(&s_guard);
    vTaskDelete(NULL);
}

void app_camera_start(int machine, int x, int y, int width, int height) {
    if (machine < 0 || machine >= app_mqtt_machine_count() || x != 0 || width < 1 || height < 1) return;
    portENTER_CRITICAL(&s_guard);
    s_generation++; if (!s_generation) s_generation++;
    s_requested = true; s_refresh = false; s_machine=machine;
    s_x=x; s_y=y; s_width=width; s_height=height;
    memset(&s_state, 0, sizeof(s_state)); s_state.status = CAMERA_CONNECTING;
    bool create = !s_task && !s_starting;
    if (create) s_starting = true;
    portEXIT_CRITICAL(&s_guard);
    if (create) {
        TaskHandle_t task;
        BaseType_t result = xTaskCreate(worker, "camera", 7168, NULL, 3, &task);
        portENTER_CRITICAL(&s_guard);
        s_starting = false;
        if (result == pdPASS) s_task = task;
        else s_state.status = CAMERA_MEMORY_ERROR;
        portEXIT_CRITICAL(&s_guard);
    }
}

void app_camera_stop(void) {
    portENTER_CRITICAL(&s_guard);
    s_requested = false; s_generation++; s_state.status = CAMERA_OFF;
    portEXIT_CRITICAL(&s_guard);
    bsp_lvgl_stream_end(0); // Caller is the LVGL page owner.
}

void app_camera_refresh(void) {
    portENTER_CRITICAL(&s_guard);
    s_refresh = true;
    bool restart = s_requested && !s_task && !s_starting;
    int machine=s_machine, x=s_x, y=s_y, width=s_width, height=s_height;
    portEXIT_CRITICAL(&s_guard);
    if (restart) app_camera_start(machine, x, y, width, height);
}

void app_camera_snapshot(camera_state_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_guard); *out = s_state; portEXIT_CRITICAL(&s_guard);
}

bool app_camera_active(void) {
    portENTER_CRITICAL(&s_guard); bool active = s_requested; portEXIT_CRITICAL(&s_guard);
    return active;
}

#include "camera_review_common.h"
#include "app_camera.h"
#include "app_camera_config.h"
#include "camera_geometry.h"
#define JSON_LIMIT 1024
typedef void esp_tls_t;
typedef struct { int unused; } mbedtls_md5_context;
typedef struct { int timeout_ms; const char *common_name; int (*crt_bundle_attach)(void *); } esp_tls_cfg_t;
static bool s_requested = true;
static uint32_t s_generation = 7;
/* @READER@ */
static camera_reader_t *s_verifying;
static int saves;
static bool save_ok, identity_ok = true, certificate_seen = true;
static uint8_t persisted[32];
static void *esp_tls_init(void) { return (void *)1; }
static int attach_verifier(void *config) { return 0; }
static int esp_tls_conn_new_sync(const char *host, int length, int port, const esp_tls_cfg_t *config, esp_tls_t *tls) {
    s_verifying->certificate_seen = certificate_seen;
    s_verifying->identity_failed = !identity_ok;
    memset(s_verifying->candidate_pin, 0x42, 32);
    return 1;
}
bool camera_config_save(const char *serial, const camera_config_t *config) {
    saves++;
    if (save_ok) memcpy(persisted, config->certificate_sha256, 32);
    return save_ok;
}
/* @CURRENT@ */
/* @CONNECT@ */
int main(int argc, char **argv) {
    assert(argc == 2);
    camera_reader_t reader = {.token=7};
    strlcpy(reader.serial, "TEST-PRINTER", sizeof(reader.serial));
    if (!strcmp(argv[1], "retry")) {
        uint8_t zero[32] = {0};
        assert(!connect_camera(&reader) && saves == 1);
        assert(!memcmp(reader.config.certificate_sha256, zero, 32));
        assert(!connect_camera(&reader) && saves == 2);
        assert(!memcmp(reader.config.certificate_sha256, zero, 32));
        save_ok = true;
        assert(connect_camera(&reader) && saves == 3);
        assert(!memcmp(reader.config.certificate_sha256, persisted, 32));
        assert(connect_camera(&reader) && saves == 3);
    } else if (!strcmp(argv[1], "identity")) {
        identity_ok = false; save_ok = true;
        assert(!connect_camera(&reader) && saves == 0 && reader.error == CAMERA_IDENTITY_ERROR);
    } else {
        certificate_seen = false; save_ok = true;
        assert(!connect_camera(&reader) && saves == 0 && reader.error == CAMERA_IDENTITY_ERROR);
    }
    return 0;
}

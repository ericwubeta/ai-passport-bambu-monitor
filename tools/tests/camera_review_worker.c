#include "camera_review_common.h"
#include "app_camera.h"
#include "app_camera_config.h"
#include "camera_geometry.h"
#define JSON_LIMIT 1024
#define CAMERA_SUCCESS_DELAY_MS 2000
typedef void *TaskHandle_t;
typedef void esp_tls_t;
typedef struct { int unused; } mbedtls_md5_context;
static TaskHandle_t s_task = (void *)1;
static bool s_starting, s_requested = true, s_refresh;
static uint32_t s_generation = 7;
static int s_machine, s_x, s_y = 92, s_width = 240, s_height = 135;
static camera_state_t s_state;
static int attempts, fetches;
static const char *scenario;
/* @READER@ */
static camera_reader_t *s_verifying;
static unsigned closed;
static int esp_tls_conn_destroy(esp_tls_t *tls) { closed++; return 0; }
/* @CLOSE@ */
/* @CURRENT@ */
/* @STATUS@ */
static bool app_wifi_connected(void) {
    attempts++;
    return strcmp(scenario, "first-offline") && (attempts == 1 || strcmp(scenario, "offline"));
}
static const char *app_mqtt_sn(int machine) { return "TEST-PRINTER"; }
static const char *app_mqtt_name(int machine) { return "Test"; }
bool camera_config_load(const char *serial, camera_config_t *out) {
    memset(out, 0, sizeof(*out));
    strlcpy(out->host, "printer.test", sizeof(out->host));
    strlcpy(out->access_code, "TEST1234", sizeof(out->access_code));
    out->transport = CAMERA_SNAPSHOT;
    return true;
}
static bool fetch(camera_reader_t *reader, bool jpeg) {
    fetches++;
    reader->tls = (void *)1;
    reader->geometry = (camera_geometry_t){.source_w=1920,.source_h=1080,.width=240,.height=135};
    reader->drew = fetches == 1 || !strcmp(scenario, "partial");
    return fetches == 1;
}
static void vTaskDelay(int ticks) {
    if (attempts >= (!strcmp(scenario,"first-offline") ? 1 : 2)) s_requested = false;
}
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
static void vTaskDelete(void *task) {}
/* @WORKER@ */
int main(int argc, char **argv) {
    assert(argc == 2); scenario = argv[1]; worker(NULL);
    if (!strcmp(scenario,"first-offline")) {
        assert(fetches == 0 && !s_state.has_image && s_state.frames == 0 && s_state.updated_us == 0);
    } else {
        assert(s_state.frames == 1 && s_state.updated_us == 1000);
        assert(s_state.source_w == 1920 && s_state.source_h == 1080);
        assert(s_state.has_image == (strcmp(scenario,"partial") != 0));
        assert(fetches == (!strcmp(scenario,"offline") ? 1 : 2));
    }
    assert(s_state.status == CAMERA_RETRYING);
    assert(closed == (fetches ? 1u : 0u));
    return 0;
}

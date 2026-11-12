// Actual app_mqtt.c with deterministic transport/config fixtures; no network.
#include "app_mqtt.h"
#include "app_config.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int machine_count;
static unsigned sent;
static unsigned subscriptions;
static unsigned sent_by_machine[4];
static unsigned char client_token;
static unsigned char mutex_token;
static esp_event_handler_t event_handler;
static void *event_argument;
static esp_timer_create_args_t timer_args;
static uint64_t timer_period;
static int64_t clock_us = 1000000;

void host_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
uint32_t esp_get_free_heap_size(void) { return 65536; }
int esp_crt_bundle_attach(void *configuration) { (void)configuration; return ESP_OK; }
int64_t esp_timer_get_time(void) { return clock_us; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex_token; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t lock, uint32_t timeout)
{ assert(lock == &mutex_token); (void)timeout; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t lock)
{ assert(lock == &mutex_token); return pdTRUE; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer)
{ timer_args = *args; *timer = &timer_args; return ESP_OK; }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us)
{ assert(timer == &timer_args); timer_period = period_us; return ESP_OK; }

bool config_load(device_config_t *config)
{
    memset(config, 0, sizeof(*config));
    strcpy(config->bambu_uid, "123");
    strcpy(config->bambu_token, "host-fixture-token");
    for (int i = 0; i < machine_count; i++) {
        snprintf(config->bambu_sn[i], sizeof(config->bambu_sn[i]), "CORE-HOST-%d", i + 1);
        snprintf(config->bambu_name[i], sizeof(config->bambu_name[i]), "Fixture%d", i + 1);
    }
    return true;
}

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config)
{
    assert(config->broker.address.transport == MQTT_TRANSPORT_OVER_SSL);
    assert(config->broker.address.port == 8883);
    assert(!strcmp(config->credentials.username, "u_123"));
    assert(!strcmp(config->credentials.authentication.password, "host-fixture-token"));
    return &client_token;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, esp_mqtt_event_id_t event,
                                       esp_event_handler_t handler, void *argument)
{
    assert(client == &client_token && event == MQTT_EVENT_ANY);
    event_handler = handler;
    event_argument = argument;
    return ESP_OK;
}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{ assert(client == &client_token); return ESP_OK; }

static int topic_machine(const char *topic, const char *suffix)
{
    for (int i = 0; i < machine_count; i++) {
        char expected[64];
        snprintf(expected, sizeof(expected), "device/CORE-HOST-%d/%s", i + 1, suffix);
        if (!strcmp(topic, expected)) return i;
    }
    assert(!"Unexpected MQTT topic");
    return -1;
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos)
{
    assert(client == &client_token && qos == 0);
    topic_machine(topic, "report");
    return (int)++subscriptions;
}
int esp_mqtt_client_publish(esp_mqtt_client_handle_t client, const char *topic, const char *data,
                            int length, int qos, int retain)
{
    assert(client == &client_token && length == 0 && qos == 0 && retain == 0);
    int index = topic_machine(topic, "request");
    cJSON *root = cJSON_Parse(data);
    assert(root && cJSON_IsObject(root) && cJSON_GetArraySize(root) == 1);
    cJSON *pushing = cJSON_GetObjectItemCaseSensitive(root, "pushing");
    assert(cJSON_IsObject(pushing) && cJSON_GetArraySize(pushing) == 1);
    cJSON *command = cJSON_GetObjectItemCaseSensitive(pushing, "command");
    assert(cJSON_IsString(command) && !strcmp(command->valuestring, "pushall"));
    assert(!cJSON_HasObjectItem(root, "print") && !cJSON_HasObjectItem(root, "system"));
    cJSON_Delete(root);
    sent_by_machine[index]++;
    return (int)++sent;
}

static void fire(esp_mqtt_event_id_t event, esp_mqtt_event_t *data)
{ assert(event_handler); event_handler(event_argument, "host-mqtt", event, data); }

static void exercise(int count)
{
    machine_count = count;
    sent = subscriptions = 0;
    memset(sent_by_machine, 0, sizeof(sent_by_machine));
    app_mqtt_start();
    assert(app_mqtt_machine_count() == count);
    assert(timer_period == 30000000ULL);
    assert(sent == 0);
    fire(MQTT_EVENT_CONNECTED, NULL);
    assert(subscriptions == (unsigned)count && sent == (unsigned)count);
    for (int i = 0; i < count; i++) app_mqtt_request_now(i);
    assert(sent == (unsigned)(count * 2));
    app_mqtt_request_now(-1);
    app_mqtt_request_now(count);
    app_mqtt_request_now(999);
    assert(sent == (unsigned)(count * 2));
    timer_args.callback(timer_args.arg);
    assert(sent == (unsigned)(count * 3));
    for (int i = 0; i < count; i++) assert(sent_by_machine[i] == 3);

    char topic[] = "device/CORE-HOST-1/report";
    char payload[] = "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":37,\"mc_remaining_time\":21,\"nozzle_temper\":210,\"bed_temper\":60}}";
    esp_mqtt_event_t data = {0};
    data.topic = topic;
    data.topic_len = (int)strlen(topic);
    data.data = payload;
    data.data_len = data.total_data_len = (int)strlen(payload);
    fire(MQTT_EVENT_DATA, &data);
    bambu_state_t state;
    app_mqtt_snapshot(0, &state);
    assert(!strcmp(state.gcode_state, "RUNNING") && state.pct == 37);
    assert(state.nozzle_t == 210 && state.bed_t == 60 && state.remain_min == 21);
    assert(sent == (unsigned)(count * 3));
    if (count > 1) { app_mqtt_snapshot(1, &state); assert(state.pct == 0); }
    fire(MQTT_EVENT_DISCONNECTED, NULL);
    app_mqtt_snapshot(0, &state);
    assert(state.cloud == BAMBU_CLOUD_CONNECTING);
    fire(MQTT_EVENT_CONNECTED, NULL);
    assert(sent == (unsigned)(count * 4) && subscriptions == (unsigned)(count * 2));
    printf("PASS: %d printers, connect/manual/timer/reconnect emit only pushall; telemetry retained\n", count);
}

int main(void)
{
    app_mqtt_request_now(0);
    assert(sent == 0);
    for (int count = 1; count <= 4; count++) exercise(count);
    return 0;
}

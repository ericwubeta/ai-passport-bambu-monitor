// main/app_mqtt.c —— 拓竹云(中国区)MQTT:订阅 device/<sn>/report,
// 每 30 秒发布 pushall 拉全量状态,轻量 JSON 扫描(游标式,内存 O(1))
// 解析进受互斥锁保护的快照。全量报文可达 11KB+,cJSON 建树会超 C3 堆。
//
// 连接参数(已实测可用):cn.mqtt.bambulab.com:8883 TLS,
// username = u_<USER_ID>,password = accessToken,client_id 随机 bbl_xxx。
#include "app_mqtt.h"
#include "app_telemetry.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "app_config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"

#include "secrets.h"

static const char *TAG = "app_mqtt";

#define BROKER_HOST "cn.mqtt.bambulab.com"
#define PUSH_PERIOD_US (30ULL * 1000000ULL)

static esp_mqtt_client_handle_t s_cli;
static SemaphoreHandle_t s_lock;
static bambu_state_t s_state[BAMBU_MACHINE_MAX];   // 每台机器独立状态槽
static int64_t s_conn_since;      // 云连接建立的时间戳(us)
static bool s_ever_data[BAMBU_MACHINE_MAX];

// 凭据:NVS(配网写入)优先,空则回落 secrets.h。app_mqtt_start 时解析一次。
static char s_uid[40];            // "u_<USER_ID>"
static char s_tok[260];           // accessToken
static char s_sn[BAMBU_MACHINE_MAX][34];   // 各打印机序列号
static char s_name[BAMBU_MACHINE_MAX][17]; // 自定义名称(可空)
static int  s_mcount;             // 有效机器数 1..4

static const char *sn_at(int i) { return s_sn[i]; }

// topic → 机器下标:device/<SN>/report 形态,匹配配置的 SN 列表
static int idx_from_topic(const char *topic, int tlen)
{
    if (!topic || tlen < 15 || memcmp(topic, "device/", 7)) return -1;
    const char *p = topic + 7;
    const char *slash = memchr(p, '/', (size_t)(topic + tlen - p));
    if (!slash || topic + tlen - slash != 7 || memcmp(slash, "/report", 7)) return -1;
    int snlen = slash ? (int)(slash - p) : (int)(topic + tlen - p);
    for (int i = 0; i < s_mcount; i++) {
        if ((int)strlen(s_sn[i]) == snlen && !strncmp(s_sn[i], p, (size_t)snlen))
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------- 工具

static void state_reset_online_part(bambu_state_t *st)
{
    st->gcode_state[0] = 0;
    st->state_received = false;
    st->state_updated_ms = 0;
    st->print_error = 0;
    st->job_generation = 0;
    st->task_id[0] = 0;
    st->pct = 0;
    st->remain_min = 0;
    st->layer = st->total_layer = 0;
    st->nozzle_t = st->nozzle_target = 0;
    st->bed_t = st->bed_target = 0;
    st->chamber_t = 0;
    st->fan_cooling_pct = 0;
    st->spd_lvl = 0;
    st->wifi_signal = 0;
    st->job[0] = 0;
    st->printer_ip = 0;
    st->ams_count = 0;
    st->ams_active = false;
    memset(st->ams, 0, sizeof(st->ams));
    st->hms_count = 0;
    memset(st->hms, 0, sizeof(st->hms));
}

static void parse_report(int idx, const char *data, int len)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        app_telemetry_merge(&s_state[idx], data, len, (uint64_t)(esp_timer_get_time() / 1000));
        s_ever_data[idx] = true;
        xSemaphoreGive(s_lock);
    }
}

// ---------------------------------------------------------------- 事件

static void push_pushall(int idx)
{
    if (!s_cli || idx < 0 || idx >= s_mcount) return;
    char topic[48], payload[40];
    snprintf(topic, sizeof(topic), "device/%s/request", sn_at(idx));
    snprintf(payload, sizeof(payload), "{\"pushing\":{\"command\":\"pushall\"}}");
    esp_mqtt_client_publish(s_cli, topic, payload, 0, 0, 0);
}

static void on_mqtt(void *handler_arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "拓竹云已连接(机器数=%d)", s_mcount);
        s_conn_since = esp_timer_get_time();
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < s_mcount; i++) {
                s_state[i].cloud = BAMBU_CLOUD_ONLINE;
                s_state[i].state_received = false;
                s_state[i].print_error = 0;
                app_telemetry_invalidate(&s_state[i]);
            }
            xSemaphoreGive(s_lock);
        }
        // 每台机器各订阅 report topic,否则收不到推送
        for (int i = 0; i < s_mcount; i++) {
            char rt[48];
            snprintf(rt, sizeof(rt), "device/%s/report", sn_at(i));
            esp_mqtt_client_subscribe(s_cli, rt, 0);
            ESP_LOGI(TAG, "已订阅 %s", rt);
            push_pushall(i);
            // Read-only metadata request, once per connection. No printer control.
            char request[64]; snprintf(request, sizeof(request), "device/%s/request", sn_at(i));
            esp_mqtt_client_publish(s_cli, request, "{\"info\":{\"sequence_id\":\"0\",\"command\":\"get_version\"}}", 0, 0, 0);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "拓竹云断开,自动重连");
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < s_mcount; i++) {
                s_state[i].cloud = BAMBU_CLOUD_CONNECTING;
                s_state[i].state_received = false;
                s_state[i].print_error = 0;
                app_telemetry_invalidate(&s_state[i]);
            }
            xSemaphoreGive(s_lock);
        }
        break;
    case MQTT_EVENT_DATA: {
        // 诊断:大报文在 esp-mqtt 里可能分段回调(total_data_len>data_len)
        if (e->total_data_len != e->data_len)
            ESP_LOGW(TAG, ">>> MQTT 分段: data_len=%d total=%d off=%d",
                     e->data_len, e->total_data_len, e->current_data_offset);
        // 按 topic 里的 SN 路由到对应机器的解析槽
        if (e->current_data_offset != 0 || e->total_data_len != e->data_len) break;
        int idx = idx_from_topic(e->topic, e->topic_len);
        if (idx >= 0) parse_report(idx, e->data, e->data_len);
        break;
    }
    default:
        break;
    }
}

static void push_timer_cb(void *arg)
{
    for (int i = 0; i < s_mcount; i++) push_pushall(i);
}

// ---------------------------------------------------------------- API

void app_mqtt_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < BAMBU_MACHINE_MAX; i++) {
        state_reset_online_part(&s_state[i]);
        s_state[i].cloud = BAMBU_CLOUD_OFF;
    }

    // 凭据解析:NVS 优先,空则回落 secrets.h(老机器兼容)。
    device_config_t cfgc;
    config_load(&cfgc);
    if (cfgc.bambu_uid[0]) {
        // NVS 存 "u_<id>" 或纯 id,统一成 "u_<id>"
        if (!strncmp(cfgc.bambu_uid, "u_", 2))
            strlcpy(s_uid, cfgc.bambu_uid, sizeof(s_uid));
        else
            snprintf(s_uid, sizeof(s_uid), "u_%s", cfgc.bambu_uid);
    } else {
        snprintf(s_uid, sizeof(s_uid), "u_%s", CFG_BAMBU_USER_ID);
    }
    if (cfgc.bambu_token[0]) strlcpy(s_tok, cfgc.bambu_token, sizeof(s_tok));
    else                     strlcpy(s_tok, CFG_BAMBU_TOKEN, sizeof(s_tok));

    // 打印机列表:SN/名称按槽位拷贝;NVS 全空则回落 secrets.h 单机。
    s_mcount = 0;
    for (int i = 0; i < BAMBU_MACHINE_MAX; i++) {
        if (!cfgc.bambu_sn[i][0]) continue;
        bool duplicate = false;
        for (int j = 0; j < s_mcount; j++) if (!strcmp(s_sn[j], cfgc.bambu_sn[i])) duplicate = true;
        if (duplicate) continue;
        strlcpy(s_sn[s_mcount], cfgc.bambu_sn[i], sizeof(s_sn[0]));
        strlcpy(s_name[s_mcount], cfgc.bambu_name[i], sizeof(s_name[0]));
        s_mcount++;
    }
    if (!s_mcount) {
        strlcpy(s_sn[0], CFG_BAMBU_SERIAL, sizeof(s_sn[0]));
        s_mcount = 1;
    }

    // ===== 多机仿真测试开关(发版前必须置 0)=====
    // >0 时为单机环境注入假机器:真机数据仍进槽 0,假机器显示"等待数据",
    // 用于单机环境验证多机概览 UI/按键导航/名称显示。设 1/2/3 看 2/3/4 台布局。
#ifndef TEST_EXTRA_MACHINES
#define TEST_EXTRA_MACHINES 0
#endif
#if TEST_EXTRA_MACHINES > 0
    for (int i = 0; i < TEST_EXTRA_MACHINES && s_mcount < BAMBU_MACHINE_MAX; i++) {
        snprintf(s_sn[s_mcount], sizeof(s_sn[0]), "TESTFAKE%07d", i + 1);
        snprintf(s_name[s_mcount], sizeof(s_name[0]), "测试%d号机", i + 2);
        s_mcount++;
    }
    ESP_LOGW(TAG, "多机仿真测试:注入 %d 台假机器,共 %d 台(发版前置 0)",
             TEST_EXTRA_MACHINES, s_mcount);
#endif

    bool configured = s_tok[0] && strncmp(s_tok, "paste", 5) != 0;
    if (!configured) {
        ESP_LOGW(TAG, "没有拓竹 token(查 NVS/secrets.h),跳过云连接。进'配网'填写账号。");
        return;
    }

    char cid[20];
    snprintf(cid, sizeof(cid), "bbl_%06X", (unsigned)(esp_timer_get_time() >> 10) & 0xFFFFFF);

    esp_mqtt_client_config_t cfg = { 0 };
    cfg.broker.address.hostname = BROKER_HOST;
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    cfg.broker.address.port = 8883;
    cfg.broker.verification.crt_bundle_attach = &esp_crt_bundle_attach;
    cfg.credentials.username = s_uid;
    cfg.credentials.authentication.password = s_tok;
    cfg.credentials.client_id = cid;   // IDF 5.5: client_id 在 credentials 下
    cfg.buffer.size = 13 * 1024;       // 全量 AMS 报文约 11.4KB,留余量;越小越省堆
    cfg.buffer.out_size = 1024;
    cfg.session.keepalive = 60;

    s_cli = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_cli, MQTT_EVENT_ANY, on_mqtt, NULL);
    esp_mqtt_client_start(s_cli);

    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < s_mcount; i++) s_state[i].cloud = BAMBU_CLOUD_CONNECTING;
        xSemaphoreGive(s_lock);
    }

    const esp_timer_create_args_t t = { .name = "bambu_push", .callback = push_timer_cb };
    esp_timer_handle_t h;
    esp_timer_create(&t, &h);
    esp_timer_start_periodic(h, PUSH_PERIOD_US);
    ESP_LOGI(TAG, "MQTT 启动:%s user=%s 机器=%d 台", BROKER_HOST, s_uid, s_mcount);
}

int app_mqtt_machine_count(void)
{
    return s_mcount;
}

void app_mqtt_request_now(int idx)
{
    push_pushall(idx);
}

void app_mqtt_snapshot(int idx, bambu_state_t *out)
{
    if (!out) return;
    if (!s_lock || idx < 0 || idx >= BAMBU_MACHINE_MAX) {
        memset(out, 0, sizeof(*out));
        out->cloud = BAMBU_CLOUD_OFF;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state[idx];
    xSemaphoreGive(s_lock);
}

int app_mqtt_uptime_sec(void)
{
    if (s_conn_since == 0) return 0;
    return (int)((esp_timer_get_time() - s_conn_since) / 1000000LL);
}

const char *app_mqtt_sn(int idx)
{
    return (idx >= 0 && idx < BAMBU_MACHINE_MAX) ? s_sn[idx] : "";
}

const char *app_mqtt_name(int idx)
{
    if (idx < 0 || idx >= BAMBU_MACHINE_MAX) return NULL;
    return s_name[idx][0] ? s_name[idx] : NULL;
}

// main/app_mqtt.h —— 拓竹云 MQTT 客户端与打印机状态快照模型(多机版)。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "nozzle_schema_decode.h"

#define BAMBU_AMS_MAX   4          // 最多同时挂载的 AMS 台数
#define BAMBU_TRAY_MAX  4          // 每台 AMS 4 个料槽
#define BAMBU_HMS_MAX   4          // 一次最多记录 4 个 HMS 错误码
#define BAMBU_MACHINE_MAX 4        // 同账号最多监控 4 台打印机

typedef enum {
    BAMBU_CLOUD_OFF = 0,   // 未启动/未配置
    BAMBU_CLOUD_CONNECTING,
    BAMBU_CLOUD_ONLINE,
} bambu_cloud_t;

typedef struct {
    char      tray_type[8];    // PLA/PETG/...(X1/P1)
    char      tray_name[24];   // tray_id_name(X2D/AMS2 无 tray_type,料名/槽名)
    uint32_t  tray_color;      // 0xRRGGBB
    bool      color_valid;     // tray_color 有效(区分"无色"与纯黑 000000FF)
    // 拓竹 ams tray 的 remain 是**剩余百分比(0~100)**,不是克;整卷克数在
    // tray_weight(如 "1000")。空槽 remain = -1 且 tray_weight = "0"。
    int       tray_pct;        // 剩余百分比 0~100;负数 = 空槽(无料卷)
    int       tray_weight;     // 整卷标称克数(如 1000),0 = 未知
    int       tray_remain;     // 剩余克数 = tray_weight * tray_pct / 100
    bool      is_used;         // 槽内有料(用于 2D 封面料斗 / 空槽置灰)
    bool      active;          // 当前打印正在使用的槽
    bool      reported;
    bool      pct_valid;
    uint64_t  updated_ms, pct_updated_ms;
} bambu_tray_t;

typedef struct {
    bool present;              // 该 AMS 在位
    uint8_t id;                // Explicit protocol ID; array order can change.
    uint64_t humidity_updated_ms, humidity_raw_updated_ms, temp_updated_ms;
    int  humidity;             // 干燥档 1..4(非百分比),-1=未知
    int  humidity_raw;         // 湿度百分比(X2D/AMS2 报 humidity_raw,如 42)
    int  temp;                 // 料仓温度 ℃
    bambu_tray_t tray[BAMBU_TRAY_MAX];
} bambu_ams_t;

typedef struct {
    uint32_t code;             // HMS 完整错误码,如 0x03004000
} bambu_hms_t;

typedef struct {
    bambu_cloud_t cloud;

    char  gcode_state[10];   // IDLE/RUNNING/PAUSE/FAILED/FINISH
    bool state_received;     // gcode_state received during this MQTT connection
    uint64_t state_updated_ms; // monotonic receipt time, not wall-clock/SNTP
    uint32_t print_error;    // 32-bit print error; independent of truncated HMS display codes
    uint32_t job_generation; // background job identity; survives leaving the printer UI
    char task_id[32];        // subtask_id when supplied by the printer (not a display name)
    int   pct;               // mc_percent 0-100
    bool pct_valid, remain_valid;
    uint64_t pct_ms, remain_ms;
    int   remain_min;        // mc_remaining_time(分钟)
    int   layer, total_layer;

    int   nozzle_t, nozzle_target;
    bool nozzle_valid, nozzle_target_valid, bed_valid, bed_target_valid, chamber_valid;
    uint64_t nozzle_ms, nozzle_target_ms, bed_ms, bed_target_ms, chamber_ms;
    nozzle_head_table_t heads;
    nozzle_state_bits_t extruder;
    bool structured_heads;
    uint64_t ams_updated_ms;
    int   bed_t, bed_target;
    int   chamber_t;
    char model[20];             // Actual reported/configured model only.
    int   fan_cooling_pct;   // cooling_fan_speed 0-15 档 → 百分比近似
    int   spd_lvl;           // 1 静音 2 标准 3 运动 4 疯狂
    int   wifi_signal;       // 打印机侧 dBm

    char  job[64];           // subtask_name(UTF-8 中文原样保留)
    int   printer_ip;        // net.info[0].ip 原始整数(小端拼接)

    bambu_ams_t ams[BAMBU_AMS_MAX];
    bambu_tray_t external_tray;
    int   ams_count;         // 在位 AMS 台数(0=外部料筒或未知)
    bool  ams_active;        // true=AMS 当前料槽;false=外部料筒
    bambu_hms_t hms[BAMBU_HMS_MAX];
    int   hms_count;         // 当前 HMS 错误数(0=无)
} bambu_state_t;

// 连接拓竹云并开始订阅(每台机器一个 report topic);每 30 秒给各机发 pushall。
void app_mqtt_start(void);

// 配置的机器数(非空 SN 数,1..4)。
int app_mqtt_machine_count(void);

// 立即再要一次 pushall(OK 键手动刷新时用)。
void app_mqtt_request_now(int idx);

// 线程安全取第 idx 台机器的状态快照。从未收到过数据时 cloud 为 OFF/CONNECTING。
void app_mqtt_snapshot(int idx, bambu_state_t *out);

// 云连接已维持的秒数(设备页显示用)。
int app_mqtt_uptime_sec(void);

// 第 idx 台机器的 SN / 自定义名称(NVS 优先,回落 secrets.h)。名称空则返回 NULL。
const char *app_mqtt_sn(int idx);
const char *app_mqtt_name(int idx);

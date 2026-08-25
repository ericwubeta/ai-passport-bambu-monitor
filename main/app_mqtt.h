// main/app_mqtt.h —— 拓竹云 MQTT 客户端与打印机状态快照模型。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BAMBU_AMS_MAX   4          // 最多同时挂载的 AMS 台数
#define BAMBU_TRAY_MAX  4          // 每台 AMS 4 个料槽
#define BAMBU_HMS_MAX   4          // 一次最多记录 4 个 HMS 错误码

typedef enum {
    BAMBU_CLOUD_OFF = 0,   // 未启动/未配置
    BAMBU_CLOUD_CONNECTING,
    BAMBU_CLOUD_ONLINE,
} bambu_cloud_t;

typedef struct {
    char      tray_type[8];    // PLA/PETG/...
    uint32_t  tray_color;      // 0xRRGGBB
    int       tray_remain;     // 克
    bool      is_used;         // 槽内有料(用于 2D 封面料斗 / 空槽置灰)
    bool      active;          // 当前打印正在使用的槽
} bambu_tray_t;

typedef struct {
    bool present;              // 该 AMS 在位
    int  humidity;             // 干燥档 1..4(非百分比),-1=未知
    int  temp;                 // 料仓温度 ℃
    bambu_tray_t tray[BAMBU_TRAY_MAX];
} bambu_ams_t;

typedef struct {
    uint32_t code;             // HMS 完整错误码,如 0x03004000
} bambu_hms_t;

typedef struct {
    bambu_cloud_t cloud;

    char  gcode_state[10];   // IDLE/RUNNING/PAUSE/FAILED/FINISH
    int   pct;               // mc_percent 0-100
    int   remain_min;        // mc_remaining_time(分钟)
    int   layer, total_layer;

    int   nozzle_t, nozzle_target;
    int   bed_t, bed_target;
    int   chamber_t;
    int   fan_cooling_pct;   // cooling_fan_speed 0-15 档 → 百分比近似
    int   spd_lvl;           // 1 静音 2 标准 3 运动 4 疯狂
    int   wifi_signal;       // 打印机侧 dBm

    char  job[64];           // subtask_name(UTF-8 中文原样保留)
    int   printer_ip;        // net.info[0].ip 原始整数(小端拼接)

    bambu_ams_t ams[BAMBU_AMS_MAX];
    int   ams_count;         // 在位 AMS 台数(0=外部料筒或未知)
    bool  ams_active;        // true=AMS 当前料槽;false=外部料筒
    bambu_hms_t hms[BAMBU_HMS_MAX];
    int   hms_count;         // 当前 HMS 错误数(0=无)
} bambu_state_t;

// 连接拓竹云并开始订阅;之后每 30 秒自动发 pushall 要一次全量状态。
void app_mqtt_start(void);

// 立即再要一次 pushall(OK 键手动刷新时用)。
void app_mqtt_request_now(void);

// 线程安全取一份状态快照。从未收到过数据时 cloud 为 OFF/CONNECTING。
void app_mqtt_snapshot(bambu_state_t *out);

// 云连接已维持的秒数(设备页显示用)。
int app_mqtt_uptime_sec(void);

// 打印控制命令:cmd ∈ "pause" / "resume" / "stop"(QoS 1,发布到 request topic)。
void app_mqtt_print_command(const char *cmd);

// 腔灯开关命令(on=true 亮)。X2D 支持 chamber_light。
void app_mqtt_set_chamber_light(bool on);

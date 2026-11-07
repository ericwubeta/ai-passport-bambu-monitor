// main/app_config.h —— 设备配置(NVS 持久化)。
//
// WiFi / 拓竹账号运行时可配置,存 NVS,供配网写入、app_wifi/app_mqtt 读取。
// 多机支持:同一拓竹账号下最多 4 台打印机,每台可自定义名称(如"书房X1C")。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_NVS_NS "aipass"          // NVS 命名空间
#define CFG_MACHINE_MAX 4            // 同账号最多 4 台打印机
#define CFG_WIFI_MAX 3               // 最多存 3 组 WiFi(家里 + 手机热点 + 备用)

// 设备配置:WiFi(多组) + 拓竹账号 + 打印机列表(1..4 台)。
typedef struct {
    char wifi_ssid[CFG_WIFI_MAX][33];
    char wifi_pass[CFG_WIFI_MAX][65];
    char bambu_uid[33];     // u_<USER_ID>
    char bambu_token[257];  // accessToken(较长)
    char bambu_sn[CFG_MACHINE_MAX][33];   // 各打印机序列号(非空 = 有效)
    char bambu_name[CFG_MACHINE_MAX][17]; // 自定义名称(可空,空则显示"N号机")
} device_config_t;

// 读取完整配置;任一项缺失则对应字段置空串。返回 false 仅当 NVS 打不开。
// 兼容旧数据:老版本 bambu_sn 单键迁移到槽 0。
bool config_load(device_config_t *cfg);

// 写回完整配置(配网成功后调用)。返回是否全部写入成功。
bool config_save(const device_config_t *cfg);

// 有效机器数(统计非空 SN 槽)。
int config_machine_count(const device_config_t *cfg);

// 有效 WiFi 组数(统计非空 SSID 槽)。
int config_wifi_count(const device_config_t *cfg);

// 是否已配网完成(读 configured 标志)。
bool config_is_provisioned(void);

// 标记配网完成。
void config_mark_provisioned(void);

// Last printer view, stored separately from account/network configuration.
// slot is the stable physical page type, not its changing navigation index.
typedef struct {
    uint8_t version;
    uint8_t slot;
    uint8_t overview;
    char serial[34];
} printer_view_config_t;

bool config_printer_view_load(printer_view_config_t *view);
bool config_printer_view_save(const printer_view_config_t *view);

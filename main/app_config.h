// main/app_config.h —— 设备配置(NVS 持久化)。
//
// 把原来的 secrets.h 硬编码(WiFi / 拓竹 user_id / token / 序列号)改为运行时可配置,
// 存 NVS,供配网写入、app_wifi/app_mqtt 读取。这样每个小伙伴刷机后填自己的拓竹账号,
// 不用改代码重编。
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define CFG_NVS_NS "aipass"          // NVS 命名空间

// 设备配置:WiFi + 拓竹账号。
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char bambu_uid[33];     // u_<USER_ID>
    char bambu_token[257];  // accessToken(较长)
    char bambu_sn[33];      // 打印机序列号
} device_config_t;

// 读取完整配置;任一项缺失则对应字段置空串。返回 false 仅当 NVS 打不开。
bool config_load(device_config_t *cfg);

// 写回完整配置(配网成功后调用)。返回是否全部写入成功。
bool config_save(const device_config_t *cfg);

// 是否已配网完成(读 configured 标志)。
bool config_is_provisioned(void);

// 标记配网完成。
void config_mark_provisioned(void);

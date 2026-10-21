// main/app_wifi.h —— WiFi STA 连接 + SNTP 对时。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// 初始化 NVS、netif、STA 并异步连接。SSID/密码来自 secrets.h;
// 若还是占位符则记日志并保持离线(不阻塞启动)。
void app_wifi_start(void);

bool app_wifi_connected(void);          // 已拿到 IP
void  app_wifi_ip_str(char *buf, int n);// 当前 IP 字符串,离线时 "0.0.0.0"
int   app_wifi_rssi(void);              // dBm,离线 0
// 当前实际关联的 WiFi 名称(从驱动取,不是存档里"打算连"的那个);未关联返回 NULL。
const char *app_wifi_current_ssid(void);

// 设备端手动切网:已保存网络列表 + 切换。
int  app_wifi_saved_count(void);              // 已保存组数(0..CFG_WIFI_MAX)
const char *app_wifi_saved_ssid(int idx);     // 第 idx 组 SSID,越界返回 NULL
bool app_wifi_switch_to(int idx);             // 切到第 idx 组(异步);参数无效返回 false
bool  app_wifi_time_ready(void);        // SNTP 已同步(可显示绝对完工时间)

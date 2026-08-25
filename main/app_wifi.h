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
bool  app_wifi_time_ready(void);        // SNTP 已同步(可显示绝对完工时间)

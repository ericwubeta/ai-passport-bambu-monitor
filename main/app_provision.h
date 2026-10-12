// main/app_provision.h —— 设备端配网(softAP 热点 + 网页表单)。
// 手机连热点 → 192.168.4.1 → 填 WiFi + 拓竹账号 → 存 NVS → 自动重启生效。
// 实现了 demo.h 三函数接口,配网指引屏作为菜单里的一页。
#pragma once
#include <stdbool.h>

#include "bsp_button.h"

// 是否需要进配网:NVS 没有完整配置,且 secrets.h 也没有真值(老机器兼容)。
bool app_provision_needed(void);

// 启动热点 + 网页服务器(幂等)。boot_done 里调用。
void app_provision_start(void);

// 配网指引屏(demo 页接口:enter/exit/key)
void app_provision_enter(void);
void app_provision_exit(void);
void app_provision_key(bsp_btn_t btn, bsp_btn_ev_t ev);

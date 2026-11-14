// main/app_settings.h —— 音量与屏幕亮度设置页（NVS 持久化）。
#pragma once

#include "bsp_button.h"

// 读取 NVS 中的音量/亮度。须在 nvs_flash_init 之后调用。
void app_settings_init(void);
unsigned app_settings_volume(void);
unsigned app_settings_brightness(void);
void app_settings_enter(void);
void app_settings_exit(void);
void app_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// Printer overview and detail pages: progress, camera, temperature, AMS, device,
// controls. Camera is always the second detail page; up/down navigate.
#pragma once

#include <stdbool.h>

#include "bsp_button.h"

void app_printer_enter(void);
void app_printer_exit(void);
void app_printer_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// Caller holds the LVGL lock. Pause network drawing while the screen is off;
// keep the selected page and resume its camera on wake.
void app_printer_set_suspended(bool suspended);

// 当前是否停留在机器概览列表(多机首页)。main 用它判断长按 OK 该退出还是该留。
bool app_printer_in_overview(void);

// main/app_printer.h —— 打印机状态主界面:多机概览 + 5 个详情页,上下键切换,30 秒自动刷新。
#pragma once

#include <stdbool.h>

#include "bsp_button.h"

void app_printer_enter(void);
void app_printer_exit(void);
void app_printer_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 当前是否停留在机器概览列表(多机首页)。main 用它判断长按 OK 该退出还是该留。
bool app_printer_in_overview(void);

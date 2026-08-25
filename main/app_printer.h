// main/app_printer.h —— 打印机状态主界面:4 个页面,上下键切换,30 秒自动刷新。
#pragma once

#include "bsp_button.h"

void app_printer_enter(void);
void app_printer_exit(void);
void app_printer_key(bsp_btn_t btn, bsp_btn_ev_t ev);

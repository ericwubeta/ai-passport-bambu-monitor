// main/app_wifi_menu.h —— 设备端手动切网页面(菜单第三项)。
#pragma once

#include "bsp_button.h"

void app_wifi_menu_enter(void);
void app_wifi_menu_exit(void);
void app_wifi_menu_key(bsp_btn_t btn, bsp_btn_ev_t ev);

#pragma once
#include "bsp_button.h"

void app_spirit_picker_enter(void);
void app_spirit_picker_exit(void);
/* Input is queued only; animation and widget updates run in the LVGL task. */
void app_spirit_picker_key(bsp_btn_t button, bsp_btn_ev_t event);

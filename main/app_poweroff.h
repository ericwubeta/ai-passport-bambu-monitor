#pragma once
#include <stdbool.h>

// Poll from the screen executor, including the moment the screen turns off.
void app_poweroff_poll(bool screen_off, bool wake_pending);
// False once terminal hardware teardown has committed; consume further input.
bool app_poweroff_activity(void);
bool app_poweroff_committed(void);

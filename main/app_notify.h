// main/app_notify.h —— 打印状态提示音(完成/失败/暂停)。
#pragma once
#include <stdbool.h>

typedef enum {
    NOTIFY_FINISH = 0,   // 打印完成:上行音阶
    NOTIFY_FAIL,         // 打印失败:下行音阶
    NOTIFY_PAUSE,        // 暂停:两声短促
} notify_kind_t;

// 初始化(懒创建音频任务,幂等)。由 app_printer_enter 调用。
void app_notify_init(void);

// 请求播放提示音(非阻塞,立即返回;播放放独立任务)。
void app_notify_trigger(notify_kind_t kind);
void app_notify_poll(void);
void app_notify_epoch(int machine);
void app_notify_voice(int machine, unsigned skin, unsigned kind);
// Scene-specific clips per role for the temperature / AMS / device pages.
// page: 0 = 温度, 1 = 耗材(AMS), 2 = 设备; variant: 0 = 主台词, 1 = 备用台词.
// Played only on an explicit page entry.
void app_notify_page_voice(int machine, unsigned skin, unsigned page, unsigned variant);
const char *app_notify_page_text(unsigned skin, unsigned page, unsigned variant);
// Fixed text paired with the device voice clip (same manifest as the ADPCM).
// Returns NULL for an invalid role/kind. This is text-only; no extra audio.
const char *app_notify_voice_text(unsigned skin, unsigned kind);
bool app_notify_busy(void);

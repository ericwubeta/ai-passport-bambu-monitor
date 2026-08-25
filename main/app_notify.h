// main/app_notify.h —— 打印状态提示音(完成/失败/暂停)。
#pragma once

typedef enum {
    NOTIFY_FINISH = 0,   // 打印完成:上行音阶
    NOTIFY_FAIL,         // 打印失败:下行音阶
    NOTIFY_PAUSE,        // 暂停:两声短促
} notify_kind_t;

// 初始化(懒创建音频任务,幂等)。由 app_printer_enter 调用。
void app_notify_init(void);

// 请求播放提示音(非阻塞,立即返回;播放放独立任务)。
void app_notify_trigger(notify_kind_t kind);

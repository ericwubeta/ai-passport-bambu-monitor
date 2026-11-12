#pragma once
void host_log(const char *tag, const char *format, ...);
#define ESP_LOGI host_log
#define ESP_LOGW host_log
#define ESP_LOGE host_log

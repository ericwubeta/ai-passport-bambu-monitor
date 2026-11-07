#include "app_notify.h"
#include <stdbool.h>
#include <string.h>

notify_kind_t app_notify_transition(const char *previous, const char *current) {
    if (!previous || !current || !previous[0] || !current[0] || !strcmp(previous, current)) return NOTIFY_NONE;
    bool running = !strcmp(previous, "RUNNING");
    bool active = running || !strcmp(previous, "PAUSE");
    if (active && !strcmp(current, "FINISH")) return NOTIFY_FINISH;
    if (active && !strcmp(current, "FAILED")) return NOTIFY_FAIL;
    if (running && !strcmp(current, "PAUSE")) return NOTIFY_PAUSE;
    return NOTIFY_NONE;
}

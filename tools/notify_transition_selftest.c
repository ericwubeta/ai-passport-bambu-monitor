#include "app_notify.h"
#include "esp_log.h"
#include <assert.h>

void notify_transition_selftest(void) {
    const struct {
        const char *previous, *current;
        notify_kind_t expected;
    } cases[] = {
        {NULL, "FINISH", NOTIFY_NONE}, {"", "FINISH", NOTIFY_NONE},
        {"", "FAILED", NOTIFY_NONE}, {"", "PAUSE", NOTIFY_NONE},
        {"IDLE", "FINISH", NOTIFY_NONE}, {"IDLE", "FAILED", NOTIFY_NONE},
        {"IDLE", "PAUSE", NOTIFY_NONE}, {"FINISH", "FINISH", NOTIFY_NONE},
        {"FAILED", "FINISH", NOTIFY_NONE}, {"FINISH", "RUNNING", NOTIFY_NONE},
        {"RUNNING", "RUNNING", NOTIFY_NONE}, {"RUNNING", "FINISH", NOTIFY_FINISH},
        {"PAUSE", "FINISH", NOTIFY_FINISH}, {"RUNNING", "FAILED", NOTIFY_FAIL},
        {"PAUSE", "FAILED", NOTIFY_FAIL}, {"RUNNING", "PAUSE", NOTIFY_PAUSE},
        {"PAUSE", "PAUSE", NOTIFY_NONE}, {"PAUSE", "RUNNING", NOTIFY_NONE},
        {"RUNNING", "IDLE", NOTIFY_NONE}, {"RUNNING", "", NOTIFY_NONE},
        {"RUNNING", NULL, NOTIFY_NONE}, {"PREPARE", "FINISH", NOTIFY_NONE},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        assert(app_notify_transition(cases[i].previous, cases[i].current) == cases[i].expected);
    }
    // Repeat snapshots notify once; another printer's history is independent.
    const char *history[] = {"", ""};
    assert(app_notify_transition(history[0], "RUNNING") == NOTIFY_NONE);
    history[0] = "RUNNING";
    assert(app_notify_transition(history[1], "FINISH") == NOTIFY_NONE);
    history[1] = "FINISH";
    assert(app_notify_transition(history[0], "FINISH") == NOTIFY_FINISH);
    history[0] = "FINISH";
    assert(app_notify_transition(history[0], "FINISH") == NOTIFY_NONE);
    ESP_LOGI("notify_test", "passed 22 transition cases and independent printer histories");
}

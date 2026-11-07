#include "camera_review_common.h"
#include "app_mqtt.h"
#include "app_notify.h"
#include "app_screen_policy.h"
static bambu_state_t s_state[BAMBU_MACHINE_MAX], s_prev;
static bool s_ever_data[BAMBU_MACHINE_MAX];
static atomic_uint s_screen_wake_sequence;
static int s_lock;
#define xSemaphoreTake(...) 1
#define xSemaphoreGive(...) ((void)0)
void app_notify_trigger(notify_kind_t kind) {}
/* @MQTT@ */
#define PAGE_MAX 9
#define CAMERA_SLOT 8
static bool s_ov_mode, s_prev_valid, s_dirty, s_stop_armed;
static int s_midx, s_cur, s_restore_slot, s_pages_n, s_slot[PAGE_MAX];
static int64_t s_stop_armed_at;
static int remembered_slot, remember_calls;
void app_mqtt_snapshot(int machine, bambu_state_t *out) { *out = s_state[machine]; }
static void remember_view(void) { remember_calls++; remembered_slot = s_slot[s_cur]; }
#define ov_refresh(...) ((void)0)
#define ensure_ams_pages(...) ((void)0)
#define apply_page_vis(...) ((void)0)
#define refresh_header(...) ((void)0)
#define refresh_camera(...) ((void)0)
#define refresh_status_bar(...) ((void)0)
#define refresh_progress(...) ((void)0)
#define refresh_temp(...) ((void)0)
#define refresh_ams(...) ((void)0)
#define refresh_device(...) ((void)0)
#define ctl_refresh_items(...) ((void)0)
/* @REFRESH@ */
static void report(int machine, const char *json) { parse_report(machine,json,(int)strlen(json)); }
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *scenario = argv[1];
    s_restore_slot = remembered_slot = 5; // AMS4 physical slot, not navigation index.
    s_slot[0]=0; s_slot[1]=8; s_slot[2]=1; s_slot[3]=6; s_slot[4]=7; s_pages_n=5;
    refresh_all();
    assert(s_restore_slot == 5 && s_slot[s_cur] == 5 && remember_calls == 0);
    report(0,"{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":99}}");
    refresh_all();
    assert(!s_state[0].ams_topology_known && s_restore_slot == 5 && remembered_slot == 5);
    if (!strcmp(scenario,"delta-then-topology")) {
        report(0,"{\"print\":{\"ams\":{\"ams\":[{\"humidity\":40},{\"humidity\":41},{\"humidity\":42},{\"humidity\":43}]}}}");
        refresh_all();
        assert(s_state[0].ams_topology_known && s_state[0].ams_count == 4);
        assert(s_restore_slot == -1 && s_slot[s_cur] == 5 && remember_calls == 0);
        report(0,"{\"print\":{\"mc_percent\":100}}");
        refresh_all();
        assert(s_state[0].ams_topology_known && s_state[0].ams_count == 4 && s_slot[s_cur] == 5);
    } else if (!strcmp(scenario,"nested-empty") || !strcmp(scenario,"direct-empty")) {
        report(0,!strcmp(scenario,"nested-empty") ? "{\"print\":{\"ams\":{\"ams\":[]}}}" : "{\"print\":{\"ams\":[]}}");
        refresh_all();
        assert(s_state[0].ams_topology_known && s_state[0].ams_count == 0);
        assert(s_restore_slot == -1 && s_slot[s_cur] == 0 && remembered_slot == 0 && remember_calls == 1);
    } else if (!strcmp(scenario,"partial")) {
        report(0,"{\"print\":{\"ams\":{\"tray_now\":0}}}"); refresh_all();
        assert(!s_state[0].ams_topology_known && s_restore_slot == 5 && remembered_slot == 5);
    } else if (!strcmp(scenario,"other-printer")) {
        report(1,"{\"print\":{\"ams\":[]}}"); refresh_all();
        assert(s_state[1].ams_topology_known && !s_state[0].ams_topology_known && s_restore_slot == 5);
    } else if (!strcmp(scenario,"invalid")) {
        report(0,"{\"print\":{\"ams\":null}}"); refresh_all();
        assert(!s_state[0].ams_topology_known && s_restore_slot == 5);
        report(0,"{\"print\":{\"ams\":{\"ams\":["); refresh_all();
        assert(!s_state[0].ams_topology_known && s_restore_slot == 5);
    } else assert(false);
    return 0;
}

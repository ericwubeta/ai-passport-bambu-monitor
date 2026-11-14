// Host-only contract tests. Example:
// cc -std=c11 -Wall -Wextra -Werror main/app_spirit_logic.c tests/test_app_spirit_logic.c -o test_app_spirit_logic
#include "../main/app_spirit_logic.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static spirit_status_t status(const char *state, uint64_t now)
{
    spirit_status_t result = { true, true, now, state, 0, 0, 0 };
    return result;
}

static void test_freshness(void)
{
    spirit_status_t s = status("RUNNING", 1000);
    assert(!spirit_status_fresh(NULL, 1000));
    assert(spirit_status_fresh(&s, 1000));
    assert(spirit_status_fresh(&s, 91000));
    assert(!spirit_status_fresh(&s, 91001));
    assert(!spirit_status_fresh(&s, 999));

    s.cloud_online = false;
    assert(!spirit_status_fresh(&s, 1000));
    s.cloud_online = true;
    s.state_received = false;
    assert(!spirit_status_fresh(&s, 1000));
    s = status("RUNNING", 0);
    assert(spirit_status_fresh(&s, 0));
    s = status("RUNNING", UINT64_MAX - 1000);
    assert(spirit_status_fresh(&s, UINT64_MAX));
}

static void test_status_mapping(void)
{
    static const uint32_t runout_codes[] = {
        0x03008015u,
        0x07008011u, 0x07018011u, 0x07028011u, 0x07038011u,
        0x07FE8011u, 0x07FF8011u,
        0x12008011u, 0x12018011u, 0x12028011u, 0x12038011u,
        0x12FF8011u,
        0x18008011u, 0x18018011u, 0x18028011u, 0x18038011u,
        0x18048011u, 0x18058011u, 0x18068011u, 0x18078011u,
        0x18FE8011u, 0x18FF8011u
    };
    // Manual pause, spool retraction, truncated HMS payload, and unknown
    // modules must not be mistaken for explicit filament exhaustion.
    static const uint32_t not_runout[] = {
        0, 0x03008001u, 0x03008004u, 0x07008004u, 0x00020001u,
        0x07048011u, 0x18088011u, 0xFFFFFFFFu
    };
    spirit_status_t s = status("PAUSE", 1000);
    for (size_t i = 0; i < sizeof(runout_codes)/sizeof(runout_codes[0]); i++) {
        s.print_error = runout_codes[i];
        assert(spirit_action_for_status(&s, 1000) == SPIRIT_RUNOUT);
    }
    for (size_t i = 0; i < sizeof(not_runout)/sizeof(not_runout[0]); i++) {
        s.print_error = not_runout[i];
        assert(spirit_action_for_status(&s, 1000) == SPIRIT_PAUSED);
    }
    s.print_error = 0x03008015u;
    s.gcode_state = "FAILED";
    assert(spirit_action_for_status(&s, 1000) == SPIRIT_FAILED);
    s.gcode_state = "RUNNING";
    assert(spirit_action_for_status(&s, 1000) == SPIRIT_PRINTING);
    s.gcode_state = "FINISH";
    assert(spirit_action_for_status(&s, 1000) == SPIRIT_COMPLETED);
    assert(spirit_action_for_status(&s, 91001) == SPIRIT_IDLE);
    s.cloud_online = false;
    assert(spirit_action_for_status(&s, 1000) == SPIRIT_IDLE);
    s = status("IDLE", 1000);
    assert(spirit_action_for_status(&s, 1000) == SPIRIT_IDLE);
    s.gcode_state = "PREPARE";
    assert(spirit_action_for_status(&s, 1000) == SPIRIT_IDLE);
    s.gcode_state = "UNKNOWN";
    assert(spirit_action_for_status(&s, 1000) == SPIRIT_IDLE);
    s.gcode_state = NULL;
    assert(spirit_action_for_status(&s, 1000) == SPIRIT_IDLE);
    assert(spirit_action_for_status(NULL, 1000) == SPIRIT_IDLE);
}

static spirit_sample_t sample_at(spirit_player_t *p, uint64_t tick)
{
    return spirit_player_sample(p, p->entered_ms + tick * SPIRIT_TICK_MS);
}

static void test_timing_contract(void)
{
    spirit_player_t p = { 0 };
    spirit_status_t s = status("IDLE", 1000);
    spirit_player_update(&p, &s, 1000);
    assert(p.initialized && p.entered_ms == 1000);
    assert(sample_at(&p, 16).frame_index == 0);
    assert(sample_at(&p, 17).frame_index == 1);
    assert(sample_at(&p, 18).frame_index == 0);
    assert(sample_at(&p, 25).frame_index == 0);
    int previous = sample_at(&p, 0).y_offset;
    int minimum = previous, maximum = previous;
    for (uint64_t tick = 0; tick < 51; tick++) {
        spirit_sample_t sample = sample_at(&p, tick);
        assert(sample.y_offset >= -2 && sample.y_offset <= 0);
        assert(sample.y_offset - previous >= -1);
        assert(sample.y_offset - previous <= 1);
        assert(!sample.complete);
        if (sample.y_offset < minimum) minimum = sample.y_offset;
        if (sample.y_offset > maximum) maximum = sample.y_offset;
        previous = sample.y_offset;
    }
    assert(minimum == -2 && maximum == 0);

    s = status("RUNNING", 2000);
    spirit_player_update(&p, &s, 2000);
    assert(spirit_player_sample(&p, 2159).frame_index == 0);
    assert(spirit_player_sample(&p, 2160).frame_index == 1);
    assert(sample_at(&p, 3).frame_index == 3);
    assert(sample_at(&p, 4).frame_index == 2);
    assert(sample_at(&p, 5).frame_index == 1);
    assert(sample_at(&p, 7).frame_index == 0);
    assert(sample_at(&p, 8).frame_index == 0);
    assert(sample_at(&p, 9).frame_index == 1);
    assert(sample_at(&p, 9).y_offset == 0);

    s = status("PAUSE", 3000);
    spirit_player_update(&p, &s, 3000);
    assert(sample_at(&p, 30).frame_index == 0);
    assert(sample_at(&p, 31).frame_index == 1);
    assert(sample_at(&p, 32).frame_index == 0);
    s.print_error = 0x03008015u;
    spirit_player_update(&p, &s, 4000);
    assert(sample_at(&p, 3).frame_index == 0);
    assert(sample_at(&p, 4).frame_index == 1);
    assert(sample_at(&p, 5).frame_index == 1);
    assert(sample_at(&p, 6).frame_index == 0);
    assert(sample_at(&p, 12).frame_index == 0);

    s = status("FAILED", 5000);
    spirit_player_update(&p, &s, 5000);
    assert(sample_at(&p, 9).frame_index == 0);
    assert(sample_at(&p, 10).frame_index == 1);
    assert(sample_at(&p, 11).frame_index == 0);
    assert(sample_at(&p, 18).frame_index == 0);

    s = status("FINISH", 6000);
    spirit_player_update(&p, &s, 6000);
    assert(sample_at(&p, 2).frame_index == 1);
    assert(sample_at(&p, 3).frame_index == 2);
    assert(sample_at(&p, 4).frame_index == 3);
    assert(sample_at(&p, 11).frame_index == 3);
    assert(sample_at(&p, 12).frame_index == 2);
    assert(sample_at(&p, 13).frame_index == 1);
    assert(sample_at(&p, 14).frame_index == 0);
    assert(!spirit_player_sample(&p, 9839).complete);
    assert(spirit_player_sample(&p, 9840).complete);
    assert(spirit_player_sample(&p, UINT64_MAX).complete);
    assert(spirit_player_sample(&p, UINT64_MAX).frame_index == 0);
    assert(!spirit_player_sample(&p, 5999).complete);
}

static void test_repeated_updates_and_reconnect(void)
{
    spirit_player_t p = { 0 };
    spirit_status_t s = status("RUNNING", 1000);
    spirit_player_update(&p, &s, 1000);
    s.state_updated_ms = 2000;
    spirit_player_update(&p, &s, 2000);
    assert(p.action == SPIRIT_PRINTING && p.entered_ms == 1000);

    s = status("FINISH", 3000);
    spirit_player_update(&p, &s, 3000);
    assert(p.action == SPIRIT_COMPLETED && p.entered_ms == 3000);
    s.state_updated_ms = 3500;
    spirit_player_update(&p, &s, 3500);
    assert(p.entered_ms == 3000);
    s.cloud_online = false;
    spirit_player_update(&p, &s, 3600);
    assert(p.action == SPIRIT_IDLE);
    assert(!spirit_player_acknowledge(&p, &s, 3600));
    s.cloud_online = true;
    s.state_updated_ms = 4000;
    spirit_player_update(&p, &s, 4000);
    assert(p.action == SPIRIT_COMPLETED && p.entered_ms == 3000);
    assert(spirit_player_sample(&p, 4000).frame_index == 3);

    // Expiry must not erase the FINISH latch or accept acknowledgement.
    spirit_player_update(&p, &s, 94001);
    assert(p.action == SPIRIT_IDLE);
    assert(!spirit_player_acknowledge(&p, &s, 94001));
    s.state_updated_ms = 95000;
    spirit_player_update(&p, &s, 95000);
    assert(p.action == SPIRIT_COMPLETED && p.entered_ms == 3000);
    assert(spirit_player_sample(&p, 95000).complete);

    assert(spirit_player_acknowledge(&p, &s, 95000));
    assert(p.action == SPIRIT_IDLE);
    assert(!spirit_player_acknowledge(&p, &s, 95000));
    spirit_player_update(&p, &s, 96000);
    assert(p.action == SPIRIT_IDLE);
    s.cloud_online = false;
    spirit_player_update(&p, &s, 97000);
    s.cloud_online = true;
    s.state_updated_ms = 98000;
    spirit_player_update(&p, &s, 98000);
    assert(p.action == SPIRIT_IDLE);

    // An absent state value is not proof of a new task.
    s.gcode_state = NULL;
    spirit_player_update(&p, &s, 99000);
    s.gcode_state = "FINISH";
    spirit_player_update(&p, &s, 100000);
    assert(p.action == SPIRIT_IDLE);

    s.gcode_state = "UNKNOWN";
    spirit_player_update(&p, &s, 100100);
    s.gcode_state = "FINISH";
    spirit_player_update(&p, &s, 100200);
    assert(p.action == SPIRIT_IDLE);

    s = status("RUNNING", 101000);
    spirit_player_update(&p, &s, 101000);
    assert(!p.finish_seen && !p.finish_acknowledged);
    s = status("FINISH", 102000);
    spirit_player_update(&p, &s, 102000);
    assert(p.action == SPIRIT_COMPLETED && p.entered_ms == 102000);
}

static void test_ack_and_instance_isolation(void)
{
    spirit_player_t a = { 0 }, b = { 0 };
    spirit_status_t s = status("RUNNING", 1000);
    assert(!spirit_player_acknowledge(&a, &s, 1000));
    s = status("FINISH", 2000);
    spirit_player_update(&a, &s, 2000);
    spirit_player_update(&b, &s, 2100);
    assert(spirit_player_acknowledge(&a, &s, 2200));
    assert(a.action == SPIRIT_IDLE);
    assert(b.action == SPIRIT_COMPLETED && b.entered_ms == 2100);
    assert(!b.finish_acknowledged);
    assert(spirit_player_acknowledge(&b, &s, 2300));

    spirit_player_t first_ack = { 0 };
    assert(spirit_player_acknowledge(&first_ack, &s, 2400));
    assert(first_ack.initialized && first_ack.action == SPIRIT_IDLE);
    assert(!spirit_player_acknowledge(NULL, &s, 2400));
    spirit_player_update(NULL, &s, 2400);
    assert(spirit_player_sample(NULL, 2400).frame_index == 0);

    spirit_player_t empty = { 0 };
    assert(spirit_player_sample(&empty, 2400).y_offset == 0);
    empty.initialized = true;
    empty.action = SPIRIT_ACTION_COUNT;
    assert(spirit_player_sample(&empty, 2400).frame_index == 0);
    assert(strcmp(spirit_action_name(SPIRIT_RUNOUT), "runout") == 0);
    assert(strcmp(spirit_action_name(SPIRIT_ACTION_COUNT), "unknown") == 0);
    assert(strcmp(spirit_action_name((spirit_action_t)-1), "unknown") == 0);
}

static void test_background_job_generation(void)
{
    spirit_player_t a = { 0 }, b = { 0 };
    spirit_status_t s = status("FINISH", 1000);
    s.job_generation = 1;
    spirit_player_update(&a, &s, 1000);
    spirit_player_update(&b, &s, 1000);
    assert(spirit_player_acknowledge(&a, &s, 1100));
    assert(a.action == SPIRIT_IDLE && a.job_generation == 1);
    assert(b.action == SPIRIT_COMPLETED && !b.finish_acknowledged);

    // The page was absent for a whole new job. Only its fresh FINISH is seen.
    s = status("FINISH", 100000);
    s.job_generation = 2;
    spirit_player_update(&a, &s, 100000);
    assert(a.action == SPIRIT_COMPLETED && !a.finish_acknowledged);
    assert(a.entered_ms == 100000 && a.job_generation == 2);
    // Another printer/player must not inherit the generation or latch.
    assert(b.job_generation == 1 && b.entered_ms == 1000);
    spirit_player_update(&a, &s, 100100);
    assert(a.entered_ms == 100000);

    assert(spirit_player_acknowledge(&a, &s, 100200));
    s.job_generation = 3;
    s.cloud_online = false;
    spirit_player_update(&a, &s, 100300);
    assert(a.job_generation == 2 && a.finish_acknowledged);
    s.cloud_online = true;
    // Expired data carrying a new generation must not release the old latch.
    spirit_player_update(&a, &s, 190001);
    assert(a.job_generation == 2 && a.finish_acknowledged);
    s.state_updated_ms = 191000;
    spirit_player_update(&a, &s, 191000);
    assert(a.job_generation == 3 && a.action == SPIRIT_COMPLETED);
    assert(!a.finish_acknowledged && a.entered_ms == 191000);

    // Acknowledgement also accepts a fresh new generation without a separate
    // caller update, rather than returning early on the old generation's ack.
    assert(spirit_player_acknowledge(&a, &s, 191100));
    s.state_updated_ms = 192000;
    s.job_generation = 4;
    assert(spirit_player_acknowledge(&a, &s, 192000));
    assert(a.job_generation == 4 && a.action == SPIRIT_IDLE);
    assert(!spirit_player_acknowledge(&a, &s, 192001));
}

int main(void)
{
    test_freshness();
    test_status_mapping();
    test_timing_contract();
    test_repeated_updates_and_reconnect();
    test_ack_and_instance_isolation();
    test_background_job_generation();
    puts("app_spirit_logic: all host checks passed");
    return 0;
}


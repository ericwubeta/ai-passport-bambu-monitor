#pragma once
#include "app_mqtt.h"

// Missing fields preserve values and individual receipt times. No allocation,
// network, NVS or clock calls. Caller owns synchronization and monotonic time.
void app_telemetry_merge(bambu_state_t *state, const char *data, int len, uint64_t now_ms);
void app_telemetry_invalidate(bambu_state_t *state);
bool app_telemetry_fresh(bool valid, uint64_t receipt_ms, uint64_t now_ms);

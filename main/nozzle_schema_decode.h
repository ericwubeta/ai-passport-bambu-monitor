/* Integration draft only. No firmware or JSON dependency. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOZZLE_HEAD_CAPACITY 8u
#define NOZZLE_EXTRUDER_NONE 15u

typedef struct {
    uint16_t current_c;
    uint16_t target_c;
} nozzle_temperature_t;

typedef struct {
    uint8_t reported_count;
    uint8_t current_id;
    uint8_t target_id;
    bool current_known;
    bool target_known;
} nozzle_state_bits_t;

typedef struct {
    uint8_t id;             /* Extruder/head ID, not the array index. */
    uint8_t nozzle_id;      /* hnow; independent of the extruder ID. */
    bool nozzle_id_valid;
    bool temperature_valid;
    nozzle_temperature_t temperature;
    uint64_t updated_ms;
} nozzle_head_t;

typedef struct {
    nozzle_head_t heads[NOZZLE_HEAD_CAPACITY];
    size_t count;
    bool truncated;
} nozzle_head_table_t;

typedef struct {
    unsigned id;
    bool has_temp;
    uint32_t packed_temp;
    bool has_nozzle_id;
    unsigned nozzle_id;
} nozzle_head_patch_t;

nozzle_temperature_t nozzle_decode_packed_temperature(uint32_t value);
nozzle_state_bits_t nozzle_decode_state_bits(uint32_t value);
/* Merge one reported object by its explicit ID. Missing fields preserve old
 * values/timestamps; a new object with no temp remains unknown. No allocation.
 * IDs 0..14 are representable in this protocol; 15 is the unknown sentinel.
 * Caller decides snapshot roster removal/reconnect invalidation policies. */
bool nozzle_head_upsert(nozzle_head_table_t *table,
                        const nozzle_head_patch_t *patch, uint64_t now_ms);
const nozzle_head_t *nozzle_head_find(const nozzle_head_table_t *table, unsigned id);
const nozzle_head_t *nozzle_head_active(const nozzle_head_table_t *table,
                                       nozzle_state_bits_t state);
bool nozzle_head_temperature_fresh(const nozzle_head_t *head,
                                   uint64_t now_ms, uint64_t max_age_ms);

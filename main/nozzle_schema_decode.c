#include "nozzle_schema_decode.h"

nozzle_temperature_t nozzle_decode_packed_temperature(uint32_t value)
{
    nozzle_temperature_t result = {
        (uint16_t)(value & UINT32_C(0xffff)),
        (uint16_t)((value >> 16) & UINT32_C(0xffff))
    };
    return result;
}

nozzle_state_bits_t nozzle_decode_state_bits(uint32_t value)
{
    nozzle_state_bits_t result = {0};
    result.reported_count = (uint8_t)(value & UINT32_C(0xf));
    result.current_id = (uint8_t)((value >> 4) & UINT32_C(0xf));
    result.target_id = (uint8_t)((value >> 8) & UINT32_C(0xf));
    result.current_known = result.reported_count > 0 && result.current_id != NOZZLE_EXTRUDER_NONE;
    result.target_known = result.reported_count > 0 && result.target_id != NOZZLE_EXTRUDER_NONE;
    return result;
}

const nozzle_head_t *nozzle_head_find(const nozzle_head_table_t *table, unsigned id)
{
    if (!table || table->count > NOZZLE_HEAD_CAPACITY) return NULL;
    for (size_t i = 0; i < table->count; ++i) {
        if (table->heads[i].id == id) return &table->heads[i];
    }
    return NULL;
}

bool nozzle_head_upsert(nozzle_head_table_t *table,
                        const nozzle_head_patch_t *patch, uint64_t now_ms)
{
    if (!table || !patch || table->count > NOZZLE_HEAD_CAPACITY ||
        patch->id >= NOZZLE_EXTRUDER_NONE ||
        (patch->has_nozzle_id && patch->nozzle_id > UINT8_MAX)) return false;
    size_t index = 0;
    while (index < table->count && table->heads[index].id != patch->id) ++index;
    if (index == table->count) {
        if (table->count == NOZZLE_HEAD_CAPACITY) {
            table->truncated = true;
            return false;
        }
        table->heads[index] = (nozzle_head_t){0};
        table->heads[index].id = (uint8_t)patch->id;
        ++table->count;
    }
    nozzle_head_t *head = &table->heads[index];
    if (patch->has_temp) {
        head->temperature = nozzle_decode_packed_temperature(patch->packed_temp);
        head->temperature_valid = true;
        head->updated_ms = now_ms;
    }
    if (patch->has_nozzle_id) {
        head->nozzle_id = (uint8_t)patch->nozzle_id;
        head->nozzle_id_valid = true;
    }
    return true;
}

const nozzle_head_t *nozzle_head_active(const nozzle_head_table_t *table,
                                       nozzle_state_bits_t state)
{
    return state.current_known ? nozzle_head_find(table, state.current_id) : NULL;
}

bool nozzle_head_temperature_fresh(const nozzle_head_t *head,
                                   uint64_t now_ms, uint64_t max_age_ms)
{
    return head && head->temperature_valid && now_ms >= head->updated_ms &&
           now_ms - head->updated_ms <= max_age_ms;
}

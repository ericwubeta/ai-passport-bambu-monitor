#pragma once

#define SPIRIT_BINDING_SKIN_COUNT 6u
#include <stdbool.h>
#include <stdint.h>

#define SPIRIT_BINDING_MAX 8
typedef struct {
    char serial[34];
    uint8_t skin;
    uint8_t finish_ack;
    uint64_t finish_token;
} spirit_binding_t;
typedef struct {
    uint32_t version;
    spirit_binding_t entries[SPIRIT_BINDING_MAX];
} spirit_bindings_t;

bool spirit_bindings_valid(const spirit_bindings_t *bindings);
spirit_binding_t *spirit_binding_find(spirit_bindings_t *bindings, const char *serial);
// Only creates a missing identity. Reorder/migration cannot overwrite a choice.
spirit_binding_t *spirit_binding_ensure(spirit_bindings_t *bindings, const char *serial, uint8_t legacy_skin);
uint64_t spirit_task_token(const char *task_id);

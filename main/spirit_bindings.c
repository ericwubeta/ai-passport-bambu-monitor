#include "spirit_bindings.h"
#include <stddef.h>
#include <string.h>

bool spirit_bindings_valid(const spirit_bindings_t *b)
{
    if (!b || b->version != 1) return false;
    for (int i = 0; i < SPIRIT_BINDING_MAX; i++) {
        if (!memchr(b->entries[i].serial, 0, sizeof(b->entries[i].serial)) || b->entries[i].skin >= SPIRIT_BINDING_SKIN_COUNT) return false;
        for (int j = 0; j < i; j++) if (b->entries[i].serial[0] && !strcmp(b->entries[i].serial, b->entries[j].serial)) return false;
    }
    return true;
}

spirit_binding_t *spirit_binding_find(spirit_bindings_t *b, const char *sn)
{
    if (!b || !sn || !sn[0] || strlen(sn) >= sizeof(b->entries[0].serial)) return NULL;
    for (int i = 0; i < SPIRIT_BINDING_MAX; i++) if (!strcmp(b->entries[i].serial, sn)) return &b->entries[i];
    return NULL;
}

spirit_binding_t *spirit_binding_ensure(spirit_bindings_t *b, const char *sn, uint8_t legacy)
{
    if (!b || !sn || !sn[0] || strlen(sn) >= sizeof(b->entries[0].serial) || legacy >= SPIRIT_BINDING_SKIN_COUNT) return NULL;
    spirit_binding_t *entry = spirit_binding_find(b, sn);
    if (entry) return entry;
    for (int i = 0; i < SPIRIT_BINDING_MAX; i++) if (!b->entries[i].serial[0]) {
        entry = &b->entries[i];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->serial, sn, strlen(sn) + 1); entry->skin = legacy; b->version = 1;
        return entry;
    }
    return NULL; // Do not silently evict an earlier user's binding/history.
}

uint64_t spirit_task_token(const char *id)
{
    if (!id || !id[0] || !strcmp(id, "0")) return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    while (*id) { hash ^= (uint8_t)*id++; hash *= UINT64_C(1099511628211); }
    return hash ? hash : 1;
}

#include "spirit_dispatch.h"
#include <string.h>

static int priority(const spirit_dispatch_status_t *s, uint64_t now)
{
    if (!spirit_status_fresh(&s->status, now)) return 1;
    switch (spirit_action_for_status(&s->status, now)) {
    case SPIRIT_FAILED: return 5;
    case SPIRIT_RUNOUT: return 4;
    case SPIRIT_PAUSED: return 3;
    default: return s->hms ? 2 : 0;
    }
}

int spirit_dispatch_choose(spirit_dispatch_t *d, const spirit_dispatch_status_t *s, size_t count, uint64_t now)
{
    if (!d || !s || !count || count > 4) return -1;
    int highest = 0;
    for (size_t i = 0; i < count; i++) { int p = priority(&s[i], now); if (p > highest) highest = p; }
    bool valid = d->initialized && d->current >= 0 && (size_t)d->current < count;
    bool keep = valid && priority(&s[d->current], now) == highest && d->priority == highest && now >= d->since_ms && now - d->since_ms < 8000;
    if (keep) return d->current;
    int start = valid ? d->current : -1;
    for (size_t j = 1; j <= count; j++) {
        int i = (start + (int)j) % (int)count;
        if (priority(&s[i], now) == highest) {
            d->current = i; d->priority = highest; d->since_ms = now; d->initialized = true; return i;
        }
    }
    return -1;
}

unsigned spirit_model_family(const char *model)
{
    // Explicit names and project IDs from the frozen official model catalog.
    static const char *const a[] = {"A1 mini", "A1", "A2L", "N1", "N2S", "N9"};
    static const char *const enclosed[] = {"P1S", "P2S", "X1", "X1 Carbon", "X1C", "X1E", "X2D", "H2D", "H2D Pro", "H2S", "H2C", "H3C", "C12", "N7", "BL-P002", "BL-P001", "C13", "N6", "O1D", "O1E", "O1S", "O1C2", "O1C", "N10"};
    if (!model) return 2;
    for (size_t i=0; i<sizeof(a)/sizeof(a[0]); i++) if (!strcmp(model,a[i])) return 0;
    for (size_t i=0; i<sizeof(enclosed)/sizeof(enclosed[0]); i++) if (!strcmp(model,enclosed[i])) return 1;
    return 2; // P1P/C11 and unknown models have no duplicated generic pack.
}

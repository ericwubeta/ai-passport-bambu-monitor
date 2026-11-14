#include "spirit_event_logic.h"

int spirit_event_observe(spirit_event_latch_t *l, const spirit_status_t *s,
                         uint64_t token, bool completed_before_boot, uint64_t now)
{
    if (!l || !spirit_status_fresh(s,now)) return -1;
    spirit_action_t action=spirit_action_for_status(s,now);
    bool changed=!l->initialized || l->action!=action || l->generation!=s->job_generation || l->token!=token;
    bool new_job=l->initialized && (l->generation!=s->job_generation || (token && l->token!=token));
    bool may_finish = token ? !completed_before_boot : l->observed_active;
    if (new_job) { l->completed=false; l->spoken_mask=0; }
    if (action==SPIRIT_COMPLETED && l->completed) return -1;
    l->initialized=true; l->action=action; l->generation=s->job_generation; l->token=token;
    if (action==SPIRIT_PRINTING || action==SPIRIT_PAUSED || action==SPIRIT_RUNOUT) l->observed_active=true;
    if (!changed) return -1;
    int kind = action==SPIRIT_PRINTING ? 0 : action==SPIRIT_PAUSED ? 1 : action==SPIRIT_RUNOUT ? 4 : action==SPIRIT_COMPLETED ? 2 : action==SPIRIT_FAILED ? 3 : -1;
    if(kind<0 || (kind==2 && !may_finish)) return -1;
    if(kind==2) l->completed=true;
    // Important changes can interrupt a normal report; repeated pauses within
    // the same task obey a60s cooldown. Completion is once per task transition.
    if (kind!=2 && (l->spoken_mask & (1u<<kind)) && !new_job && now>=l->last_spoken_ms[kind] && now-l->last_spoken_ms[kind]<60000) return -1;
    l->last_spoken_ms[kind]=now; l->spoken_mask|=(uint8_t)(1u<<kind);
    return kind;
}

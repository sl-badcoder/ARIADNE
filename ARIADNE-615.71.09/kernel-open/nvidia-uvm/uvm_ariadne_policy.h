#ifndef __UVM_ARIADNE_POLICY_H__
#define __UVM_ARIADNE_POLICY_H__

#include "nvtypes.h"

#define UVM_ARIADNE_UTLB_HISTORY 16
#define UVM_ARIADNE_SD_WINDOW    100

typedef struct
{
    NvU32 ids[UVM_ARIADNE_UTLB_HISTORY];
    NvU8 count;
    NvU8 next;
    NvU8 degree;
} uvm_ariadne_utlb_history_t;

typedef struct
{
    NvU8 samples[UVM_ARIADNE_SD_WINDOW];
    NvU8 count;
    NvU8 next;
    NvU16 sum;
} uvm_ariadne_sd_window_t;

typedef struct
{
    NvU64 active_bytes;
    NvU64 capacity_bytes;
    NvU64 pinned_bytes;
} uvm_ariadne_working_set_t;

static inline unsigned uvm_ariadne_low_sharing(const uvm_ariadne_sd_window_t *window, unsigned max_average_tenths)
{
    return window->count && 10U * window->sum <= (NvU64)max_average_tenths * window->count;
}

static inline unsigned uvm_ariadne_severe_pressure(const uvm_ariadne_working_set_t *set)
{
    return set->capacity_bytes && set->active_bytes > set->capacity_bytes &&
           set->active_bytes - set->capacity_bytes > set->capacity_bytes / 10;
}

// Keep enough active data in zero-copy to fit the observed managed capacity.
// Aggressive adjustment is expressed in bytes and never exceeds active demand.
static inline NvU64 uvm_ariadne_pin_budget(const uvm_ariadne_working_set_t *set, NvU64 adjustment)
{
    NvU64 deficit;

    if (!set->capacity_bytes || set->active_bytes <= set->capacity_bytes)
        return 0;
    deficit = set->active_bytes - set->capacity_bytes;
    return adjustment > set->active_bytes - deficit ? set->active_bytes : deficit + adjustment;
}

// The 535 artifact scales pin time above 2000 active 2 MiB blocks and makes
// repeated pins five times longer. Saturate before either multiplication.
static inline NvU64 uvm_ariadne_pin_duration(NvU64 base, NvU64 active_bytes, NvU64 block_bytes, unsigned repeat)
{
    NvU64 blocks, duration, fraction;
    const NvU64 limit = ~(NvU64)0;

    if (!base || !block_bytes)
        return 0;
    blocks = active_bytes / block_bytes + !!(active_bytes % block_bytes);
    if (blocks < 2000)
        blocks = 2000;
    if (blocks / 2000 > limit / base)
        return limit;
    duration = base * (blocks / 2000);
    fraction = (base / 2000) * (blocks % 2000) + (base % 2000) * (blocks % 2000) / 2000;
    if (fraction > limit - duration)
        return limit;
    duration += fraction;
    if (repeat > 1)
        return duration > limit / 5 ? limit : duration * 5;
    return duration;
}

// Remove the oldest ID and add the new ID. An ID contributes one to the
// degree while at least one instance remains anywhere in the window.
static inline void uvm_ariadne_add_utlb(uvm_ariadne_utlb_history_t *history, NvU32 id)
{
    unsigned i;
    unsigned old_survives = 0;
    unsigned already_present = 0;

    for (i = 0; i < history->count; ++i) {
        if (i == history->next)
            continue;
        old_survives |= history->ids[i] == history->ids[history->next];
        already_present |= history->ids[i] == id;
    }

    if (history->count == UVM_ARIADNE_UTLB_HISTORY)
        history->degree -= !old_survives;
    else
        ++history->count;

    history->degree += !already_present;
    history->ids[history->next] = id;
    history->next = (history->next + 1) % UVM_ARIADNE_UTLB_HISTORY;
}

static inline void uvm_ariadne_sample_sd(uvm_ariadne_sd_window_t *window, NvU8 degree)
{
    window->sum -= window->samples[window->next];
    window->samples[window->next] = degree;
    window->sum += degree;
    if (window->count < UVM_ARIADNE_SD_WINDOW)
        ++window->count;
    window->next = (window->next + 1) % UVM_ARIADNE_SD_WINDOW;
}

// Check recent eviction and aggregate sharing. The controller may relax the
// aggregate ceiling under severe pressure; SD eviction scores still rank
// candidates. Return an absolute deadline on the native unpin clock.
static inline NvU64 uvm_ariadne_pin_deadline(NvU64 now,
                                             NvU64 eviction,
                                             NvU64 duration,
                                             NvU8 degree,
                                             const uvm_ariadne_sd_window_t *window,
                                             unsigned max_average_tenths)
{
    if (!eviction || !duration || now < eviction || now - eviction >= duration ||
        duration > ~(NvU64)0 - now || !degree || !window->count)
        return 0;
    if (!uvm_ariadne_low_sharing(window, max_average_tenths))
        return 0;
    return now + duration;
}

#endif

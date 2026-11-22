#include "uvm_perf_ariadne.h"
#include "uvm_gpu.h"
#include "uvm_global.h"
#include "uvm_va_block.h"

static unsigned uvm_perf_ariadne_enable = 1;
module_param(uvm_perf_ariadne_enable, uint, S_IRUGO);

static unsigned uvm_ariadne_zero_copy_ms = 100;
module_param(uvm_ariadne_zero_copy_ms, uint, S_IRUGO);
MODULE_PARM_DESC(uvm_ariadne_zero_copy_ms, "Recent eviction interval and CPU pin duration in ms (0 disables)");

static unsigned uvm_dynzero_thr_avg_sd = 15;
module_param(uvm_dynzero_thr_avg_sd, uint, S_IRUGO);
MODULE_PARM_DESC(uvm_dynzero_thr_avg_sd, "Maximum average sharing degree in tenths for zero-copy admission");

static unsigned uvm_perf_SD_coeff_evictqueue = 1000000;
module_param(uvm_perf_SD_coeff_evictqueue, uint, S_IRUGO);

static unsigned uvm_dynzero_aggradjust = 20;
module_param(uvm_dynzero_aggradjust, uint, S_IRUGO);
MODULE_PARM_DESC(uvm_dynzero_aggradjust, "Extra 2 MiB blocks in the low-sharing zero-copy budget");

#define UVM_ARIADNE_ACTIVE_GRACE_NS 500000000ULL

static void ariadne_touch_locked(uvm_gpu_t *gpu, uvm_va_block_gpu_state_t *state, NvU64 bytes, NvU64 now)
{
    uvm_assert_spinlock_locked(&gpu->pmm.list_lock);
    if (!state->ariadne_active_bytes) {
        state->ariadne_active_bytes = bytes;
        gpu->ariadne_working_set.active_bytes += bytes;
    }
    state->ariadne_last_active = now;
    list_move_tail(&state->ariadne_active_node, &gpu->ariadne_active_blocks);
}

void uvm_perf_ariadne_residency(uvm_gpu_t *gpu, uvm_va_block_t *block, bool resident)
{
    uvm_va_block_gpu_state_t *state = uvm_va_block_gpu_state_get(block, gpu->id);

    uvm_assert_mutex_locked(&block->lock);
    if (!uvm_perf_ariadne_enabled() || uvm_va_block_is_hmm(block) || !state)
        return;
    uvm_spin_lock(&gpu->pmm.list_lock);
    state->ariadne_resident = resident;
    if (resident || state->ariadne_active_bytes)
        ariadne_touch_locked(gpu, state, uvm_va_block_size(block), NV_GETTIME());
    uvm_spin_unlock(&gpu->pmm.list_lock);
}

void uvm_perf_ariadne_capacity_locked(uvm_gpu_t *gpu, NvU64 capacity)
{
    uvm_va_block_gpu_state_t *state, *next;
    NvU64 now = NV_GETTIME();

    uvm_assert_spinlock_locked(&gpu->pmm.list_lock);
    gpu->ariadne_working_set.capacity_bytes = capacity;
    list_for_each_entry_safe(state, next, &gpu->ariadne_active_blocks, ariadne_active_node) {
        if (!state->ariadne_resident && uvm_page_mask_empty(&state->ariadne_pinned_pages) &&
            now >= state->ariadne_last_active && now - state->ariadne_last_active >= UVM_ARIADNE_ACTIVE_GRACE_NS) {
            gpu->ariadne_working_set.active_bytes -= state->ariadne_active_bytes;
            state->ariadne_active_bytes = 0;
            list_del_init(&state->ariadne_active_node);
        }
    }
}

void uvm_perf_ariadne_forget(uvm_gpu_t *gpu, uvm_va_block_t *block)
{
    uvm_va_block_gpu_state_t *state = uvm_va_block_gpu_state_get(block, gpu->id);

    uvm_assert_mutex_locked(&block->lock);
    if (!state)
        return;
    uvm_spin_lock(&gpu->pmm.list_lock);
    gpu->ariadne_working_set.active_bytes -= state->ariadne_active_bytes;
    gpu->ariadne_working_set.pinned_bytes -= (NvU64)uvm_page_mask_weight(&state->ariadne_pinned_pages) * PAGE_SIZE;
    state->ariadne_active_bytes = 0;
    uvm_page_mask_zero(&state->ariadne_pinned_pages);
    list_del_init(&state->ariadne_active_node);
    uvm_spin_unlock(&gpu->pmm.list_lock);
}

void uvm_perf_ariadne_split(uvm_gpu_t *gpu, uvm_va_block_t *existing, uvm_va_block_t *new_block, NvU64 existing_bytes)
{
    uvm_va_block_gpu_state_t *old_state = uvm_va_block_gpu_state_get(existing, gpu->id);
    uvm_va_block_gpu_state_t *new_state = uvm_va_block_gpu_state_get(new_block, gpu->id);

    uvm_assert_mutex_locked(&existing->lock);
    uvm_assert_mutex_locked(&new_block->lock);
    UVM_ASSERT(uvm_page_mask_empty(&old_state->ariadne_pinned_pages));
    UVM_ASSERT(uvm_page_mask_empty(&new_state->ariadne_pinned_pages));
    new_state->ariadne_history = old_state->ariadne_history;
    new_state->ariadne_last_eviction = old_state->ariadne_last_eviction;
    new_state->ariadne_pin_epochs = old_state->ariadne_pin_epochs;

    uvm_spin_lock(&gpu->pmm.list_lock);
    if (old_state->ariadne_active_bytes) {
        // Splitting preserves total active bytes, including partial blocks.
        UVM_ASSERT(old_state->ariadne_active_bytes == existing_bytes + uvm_va_block_size(new_block));
        old_state->ariadne_active_bytes = existing_bytes;
        new_state->ariadne_active_bytes = uvm_va_block_size(new_block);
        new_state->ariadne_last_active = old_state->ariadne_last_active;
        list_add(&new_state->ariadne_active_node, &old_state->ariadne_active_node);
    }
    old_state->ariadne_resident = !uvm_page_mask_empty(&old_state->resident);
    new_state->ariadne_resident = !uvm_page_mask_empty(&new_state->resident);
    uvm_spin_unlock(&gpu->pmm.list_lock);
}

void uvm_perf_ariadne_pin(uvm_gpu_t *gpu, uvm_va_block_t *block, uvm_page_index_t page, NvU64 deadline)
{
    uvm_va_block_gpu_state_t *state = uvm_va_block_gpu_state_get(block, gpu->id);

    uvm_assert_mutex_locked(&block->lock);
    UVM_ASSERT(state);
    uvm_spin_lock(&gpu->pmm.list_lock);
    ariadne_touch_locked(gpu, state, uvm_va_block_size(block), NV_GETTIME());
    if (uvm_page_mask_empty(&state->ariadne_pinned_pages)) {
        if (state->ariadne_pin_epochs < 255)
            ++state->ariadne_pin_epochs;
        state->ariadne_pin_deadline = deadline;
    }
    if (!uvm_page_mask_test_and_set(&state->ariadne_pinned_pages, page)) {
        gpu->ariadne_working_set.pinned_bytes += PAGE_SIZE;
        ++gpu->ariadne_pin_events;
    }
    uvm_spin_unlock(&gpu->pmm.list_lock);
}

void uvm_perf_ariadne_unpin(uvm_va_block_t *block, uvm_gpu_id_t gpu_id, uvm_page_index_t page)
{
    uvm_va_block_gpu_state_t *state = uvm_va_block_gpu_state_get(block, gpu_id);
    uvm_gpu_t *gpu;

    uvm_assert_mutex_locked(&block->lock);
    if (!state)
        return;
    gpu = uvm_gpu_get(gpu_id);
    uvm_spin_lock(&gpu->pmm.list_lock);
    if (uvm_page_mask_test_and_clear(&state->ariadne_pinned_pages, page)) {
        gpu->ariadne_working_set.pinned_bytes -= PAGE_SIZE;
        ++gpu->ariadne_unpin_events;
    }
    if (uvm_page_mask_empty(&state->ariadne_pinned_pages))
        state->ariadne_pin_deadline = 0;
    uvm_spin_unlock(&gpu->pmm.list_lock);
}

bool uvm_perf_ariadne_enabled(void)
{
    return uvm_perf_ariadne_enable != 0;
}

void uvm_perf_ariadne_record_utlb(uvm_gpu_t *gpu, uvm_va_block_t *block, NvU32 utlb_id)
{
    uvm_va_block_gpu_state_t *state;

    if (!uvm_perf_ariadne_enabled() || uvm_va_block_is_hmm(block))
        return;

    uvm_assert_mutex_locked(&block->lock);
    // An authorized first-touch fault can precede creation of GPU block
    // state. The normal mapping path reuses this state or retries allocation.
    state = uvm_va_block_gpu_state_get_alloc(block, gpu);
    if (state)
        uvm_ariadne_add_utlb(&state->ariadne_history, utlb_id);
}

void uvm_perf_ariadne_record_migration(uvm_gpu_t *gpu, uvm_va_block_t *block)
{
    uvm_va_block_gpu_state_t *state;

    if (!uvm_perf_ariadne_enabled())
        return;

    uvm_assert_mutex_locked(&block->lock);
    state = uvm_va_block_gpu_state_get(block, gpu->id);
    if (!state || !state->ariadne_history.count)
        return;

    uvm_spin_lock(&gpu->pmm.list_lock);
    ariadne_touch_locked(gpu, state, uvm_va_block_size(block), NV_GETTIME());
    uvm_ariadne_sample_sd(&gpu->ariadne_sd_window, state->ariadne_history.degree);
    uvm_spin_unlock(&gpu->pmm.list_lock);
}

void uvm_perf_ariadne_record_eviction(uvm_gpu_t *gpu, uvm_va_block_t *block)
{
    uvm_va_block_gpu_state_t *state;

    if (!uvm_perf_ariadne_enabled() || !uvm_ariadne_zero_copy_ms)
        return;

    uvm_assert_mutex_locked(&block->lock);
    state = uvm_va_block_gpu_state_get(block, gpu->id);
    if (state && !uvm_page_mask_empty(&state->evicted))
        state->ariadne_last_eviction = NV_GETTIME();
}

NvU64 uvm_perf_ariadne_pin_deadline(uvm_gpu_t *gpu, uvm_va_block_t *block, uvm_page_index_t page, NvU64 now)
{
    uvm_va_block_gpu_state_t *state = uvm_va_block_gpu_state_get(block, gpu->id);
    NvU64 deadline, duration, budget, adjustment;
    unsigned sharing_ceiling;

    uvm_assert_mutex_locked(&block->lock);
    if (!uvm_pmm_gpu_ariadne_supported(&gpu->pmm) || !state ||
        !state->ariadne_last_eviction || !uvm_page_mask_test(&state->evicted, page))
        return 0;

    uvm_spin_lock(&gpu->pmm.list_lock);
    gpu->ariadne_working_set.capacity_bytes = gpu->pmm.ariadne.managed_bytes;
    adjustment = uvm_ariadne_low_sharing(&gpu->ariadne_sd_window, uvm_dynzero_thr_avg_sd) ?
                 (NvU64)uvm_dynzero_aggradjust * UVM_VA_BLOCK_SIZE : 0;
    budget = uvm_ariadne_pin_budget(&gpu->ariadne_working_set, adjustment);
    sharing_ceiling = uvm_ariadne_severe_pressure(&gpu->ariadne_working_set) ? UINT_MAX : uvm_dynzero_thr_avg_sd;
    deadline = uvm_ariadne_pin_deadline(now,
                                        state->ariadne_last_eviction,
                                        (NvU64)uvm_ariadne_zero_copy_ms * 1000000,
                                        state->ariadne_history.degree,
                                        &gpu->ariadne_sd_window,
                                        sharing_ceiling);
    if (!deadline || gpu->ariadne_working_set.pinned_bytes >= budget)
        deadline = 0;
    else if (!uvm_page_mask_empty(&state->ariadne_pinned_pages))
        deadline = state->ariadne_pin_deadline > now ? state->ariadne_pin_deadline : 0;
    else {
        duration = uvm_ariadne_pin_duration((NvU64)uvm_ariadne_zero_copy_ms * 1000000,
                                            gpu->ariadne_working_set.active_bytes,
                                            UVM_VA_BLOCK_SIZE,
                                            (unsigned)state->ariadne_pin_epochs + 1);
        deadline = duration <= ~(NvU64)0 - now ? now + duration : 0;
    }
    uvm_spin_unlock(&gpu->pmm.list_lock);
    return deadline;
}

NvU64 uvm_perf_ariadne_eviction_bias(uvm_gpu_t *gpu, uvm_va_block_t *block)
{
    uvm_va_block_gpu_state_t *state = uvm_va_block_gpu_state_get(block, gpu->id);

    uvm_assert_mutex_locked(&block->lock);
    if (!uvm_perf_ariadne_enabled() || !state)
        return 0;
    return ((NvU64)uvm_perf_SD_coeff_evictqueue * state->ariadne_history.degree) >> 4;
}

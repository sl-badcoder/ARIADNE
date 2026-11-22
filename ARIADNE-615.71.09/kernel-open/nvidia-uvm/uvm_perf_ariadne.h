#ifndef __UVM_PERF_ARIADNE_H__
#define __UVM_PERF_ARIADNE_H__

#include "uvm_linux.h"
#include "uvm_forward_decl.h"
#include "uvm_va_block_types.h"

bool uvm_perf_ariadne_enabled(void);

// Callers hold the VA block mutex. The eviction notification never allocates
// and does not require the VA space lock.
void uvm_perf_ariadne_record_utlb(uvm_gpu_t *gpu, uvm_va_block_t *block, NvU32 utlb_id);
void uvm_perf_ariadne_record_migration(uvm_gpu_t *gpu, uvm_va_block_t *block);
void uvm_perf_ariadne_record_eviction(uvm_gpu_t *gpu, uvm_va_block_t *block);
NvU64 uvm_perf_ariadne_eviction_bias(uvm_gpu_t *gpu, uvm_va_block_t *block);
NvU64 uvm_perf_ariadne_pin_deadline(uvm_gpu_t *gpu, uvm_va_block_t *block, uvm_page_index_t page, NvU64 now);

// All block hooks require the block mutex. Capacity sampling requires the
// GPU's PMM list_lock and prunes only nonresident, unpinned stale entries.
void uvm_perf_ariadne_residency(uvm_gpu_t *gpu, uvm_va_block_t *block, bool resident);
void uvm_perf_ariadne_forget(uvm_gpu_t *gpu, uvm_va_block_t *block);
void uvm_perf_ariadne_split(uvm_gpu_t *gpu, uvm_va_block_t *existing, uvm_va_block_t *new_block, NvU64 existing_bytes);
void uvm_perf_ariadne_capacity_locked(uvm_gpu_t *gpu, NvU64 capacity);
void uvm_perf_ariadne_pin(uvm_gpu_t *gpu, uvm_va_block_t *block, uvm_page_index_t page, NvU64 deadline);
void uvm_perf_ariadne_unpin(uvm_va_block_t *block, uvm_gpu_id_t gpu_id, uvm_page_index_t page);

#endif

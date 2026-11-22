# ARIADNE on NVIDIA 615.71.09

This is an experimental source port. Linux module linking, loading, correctness
under oversubscription, and performance still require validation on real hardware.
The original `../ARIADNE/` tree and its install/benchmark scripts are unchanged.

The base is NVIDIA's exact [615.71.09 tag](https://github.com/NVIDIA/open-gpu-kernel-modules/tree/615.71.09),
commit `61dcc93722ecb418bb5f2e00923f05b4b8051dd1` (annotated tag object
`d2a24f90e843359450ab313a286f43a88d95c6ce`). The complete corresponding
driver source is included, with its upstream licenses. The port changes UVM;
the remaining NVIDIA modules retain the upstream implementation.

The supplied 535.86.05 implementation was the primary algorithm reference.
The [published paper abstract](https://ieeexplore.ieee.org/abstract/document/11408564)
identifies pipelined fault handling, Sharing Degree, and dynamic GPU/zero-copy
placement as the three main techniques. No full paper was available in this
workspace or through the authors' publication page during this implementation.
This port does not claim to reproduce the paper's measurements.

## Implemented behavior

* Sharing Degree uses the last 16 demand-fault uTLB IDs for each block/GPU pair.
  IDs are full-width, including zero. Eligible merged fault instances are sampled;
  allocation retries, authorized faults, throttled faults, and prefetch faults do
  not add repeated observations. The GPU aggregate is a bounded 100-sample window.
* Used-root eviction uses recency plus the original Sharing Degree bias:
  `(time_ns >> 10) + ((coefficient * degree) >> 4)`. Free, unused, and discarded
  roots retain upstream priority. Current-batch roots are avoided where possible;
  synchronous allocation may fall back to them to preserve forward progress.
* Fault handling overlaps a bounded anticipatory eviction with fault servicing
  after observed memory pressure. One whole used root can be evicted per active
  GPU/VA-space segment of a batch, provided no full free root exists. Split/HMM
  roots and the active fault batch are excluded. The worker uses upstream PMM
  eviction and trackers; failures never free a failed victim. Work is drained
  before the borrowed fault cache or VA-space/GPU context can disappear, and
  canceled before PMM teardown.
* Recently evicted CPU-resident pages in eligible blocks can remain in CPU
  memory and be mapped by the GPU. Timed pinning, remote unmapping, and teardown
  use the native thrashing machinery. Synthetic pins have a separate deadline
  queue, retry failed expiry unmaps, and are unmapped before block splitting
  discards their descriptors.
* Working-set accounting retains nonresident blocks for 500 ms and tracks
  partial blocks in bytes. A pin budget covers the excess of active demand over
  PMM's managed USER capacity; low sharing adds the original 20-block adjustment.
  Above 10% oversubscription, the base deficit can use zero-copy even for high
  sharing. Capacity follows PMA ownership, including pinned and split roots.
  Pin duration scales above 2000 active 2 MiB blocks, with a fivefold increase
  for repeated pin episodes. Accounting is removed on GPU unregister and block
  destruction, and conserved across successful splits.
* Managed GPU prefetching uses the original threshold of 1 while retaining 615's
  policy checks and thrashing exclusions. CPU, HMM, and ATS thresholds remain
  upstream behavior.

Zero-copy admission currently requires one registered GPU, a managed allocation,
no explicit preferred location/accessed-by/read-duplication policy, accessible CPU
memory, a recent eviction of that page from that GPU, and compatible atomics.
HMM, ATS, integrated/NUMA-enabled GPUs, configurations without PMA eviction,
confidential computing, and CDMM use native placement.
Unsupported cases continue through the upstream fault-service paths.

## Deliberate differences from the 535 artifact

The old CPU populate/copy kthreads copied service contexts and operated without
615's required lock/lifetime guarantees. They are replaced by bounded anticipatory
eviction plus the upstream asynchronous GPU copy/tracker path. This is a narrower
overlap mechanism, not the original three-stage CPU fault pipeline.

The working-set list is embedded in per-block/GPU state rather than using the
artifact's separately allocated entries and raw VA-space references. The
controller uses byte budgets and actual eviction/refault evidence with native
per-page timed pins; the artifact selected whole regions. Deadline queues replace
the old 40 ms polling loop and its quadratic unpin quota. Failed splits retain
pin descriptors until commit, and promotion to native thrashing mitigation
replaces synthetic descriptors with native timeouts/history. These adaptations
can produce different placement and performance; the exact CPU pipeline and
paper-level performance reproduction remain unverified.

## Build on Linux

Use firmware and user-space driver components matching **615.71.09**. Changing
only kernel modules across NVIDIA driver versions is not supported by upstream.

From the repository root:

```sh
bash build_ARIADNE_615.sh
# Or select configured headers for a different kernel:
KERNEL_HEADERS=/path/to/configured/kernel/headers bash build_ARIADNE_615.sh
# Debug UVM/kernel assertions:
bash build_ARIADNE_615.sh NV_BUILD_TYPE=debug UVM_BUILD_TYPE=debug
```

The script builds all matching modules and does not install or unload drivers.
Do not use the original `Install_ARIADNE.sh` for this tree: it targets the 535
artifact. Installation/module replacement is a separate hardware-test step.

Read-only load-time parameters on `nvidia_uvm`:

| Parameter | Default | Meaning |
| --- | --- | --- |
| `uvm_perf_ariadne_enable` | 1 | Master switch; 0 restores native decisions |
| `uvm_ariadne_eviction_enable` | 1 | Sharing-aware used-root eviction selection |
| `uvm_perf_fhp` | 1 | Bounded anticipatory eviction overlap |
| `uvm_perf_SD_coeff_evictqueue` | 1000000 | Sharing Degree eviction bias |
| `uvm_dynzero_thr_avg_sd` | 15 | Low-sharing threshold in **tenths**: 15 means 1.5 |
| `uvm_dynzero_aggradjust` | 20 | Extra 2 MiB blocks in the low-sharing pin budget |
| `uvm_ariadne_zero_copy_ms` | 100 | Recent-eviction interval and base pin duration; 0 disables |

The native `uvm_perf_thrashing_enable` must also be enabled and its pin timeout
nonzero for synthetic pins. Sharing Degree prioritizes eviction candidates;
high-sharing workloads can still use the base deficit budget above 10%
oversubscription. Explicit user policy always takes precedence.

## Validation

Portable policy tests compile and pass with address and undefined-behavior
sanitizers on macOS. They exercise full-width IDs, zero, repeated IDs, ring
wraparound, aggregate replacement, the 1.4/1.5/1.6 admission boundary, expiry,
timestamp overflow, byte budgets, severe-pressure admission, and adaptive/repeated
pin durations. Run from the repository root:

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined \
  -I ARIADNE-615.71.09/kernel-open/common/inc \
  -I ARIADNE-615.71.09/kernel-open/nvidia-uvm \
  tests/ariadne_615/policy_test.c -o /tmp/ariadne-policy-test
/tmp/ariadne-policy-test
```

All 118 UVM C translation units passed debug object compilation with Clang 21
targeting Linux x86-64, Ubuntu 6.8.0-31 headers, and NVIDIA's generated feature
probes. LLD 21 linked them into a relocatable ELF `nvidia-uvm.o`; no unresolved
`uvm_*` or ARIADNE symbols remained. This is an aggregate-object check, not a full
Linux Kbuild/modpost, signed `.ko`, or runtime validation. Remaining external
Linux and NVIDIA symbols still require the native module build.
The seven changed C units plus `uvm_va_space.c` and `uvm_pmm_test.c` also passed
release object compilation.
The recorded configuration and source fingerprint are in
[`validation.json`](../tests/ariadne_615/validation.json).
The GitHub Actions workflow builds against Linux headers when run; no hosted CI
run has been performed from this workspace.

The CUDA smoke test requires Linux, the matching loaded driver, and `nvcc`:

```sh
nvcc -O2 tests/ariadne_615/managed_memory_smoke.cu -o /tmp/ariadne-managed-smoke
/tmp/ariadne-managed-smoke 256 4
# Then choose MiB above GPU memory, with enough free host RAM:
/tmp/ariadne-managed-smoke ALLOCATION_MiB 8 PAUSE_MS
```

The third argument controls the pause between rounds (default 300 ms). Select
a pause longer than the adaptive pin time to exercise expiry. With the default
base, repeated pins can last roughly `500 ms * max(1, active_MiB / 4000)`.
It verifies every element after contiguous/interleaved GPU updates and checks
an interior range split and explicit CPU placement. Run with ARIADNE enabled and with
the master switch disabled. Follow with allocation/free stress, concurrent
process exit, partial-range advice/splits, CPU/GPU atomics, and the supplied
oversubscription benchmarks. Inspect kernel logs for UVM assertions and NVIDIA
errors. These GPU/runtime tests have not been run here.

## Runtime counters

With native debug procfs enabled (`uvm_enable_debug_procfs=1` at module load),
each GPU's existing `info` entry includes active bytes, managed capacity,
pinned bytes, average SD in tenths, pin/unpin events, and anticipatory evictions:

```sh
grep -H ARIADNE /proc/driver/nvidia-uvm/gpus/*/info
```

Read these while the workload is running. Pin/unpin event counts and the
anticipatory-eviction count distinguish actual policy activity from a correctness
test that never reaches memory pressure. Active nonresident entries are aged
at eligible fault-batch boundaries, so the active-byte snapshot may retain
recently inactive demand during idle periods.

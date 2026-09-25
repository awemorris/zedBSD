# i915 contract tests

GPU-free host tests of the i915 driver's adaptation layer. Each test links the
production source it checks with a mock behind that source's operations table,
and checks the contract the source promises (return conventions, ownership,
ordering), not the hardware.

```
src/drivers/gpu/i915/tests/contracts/run.sh          # all: mmio dma pci rpm pte sync
src/drivers/gpu/i915/tests/contracts/run.sh mmio pte # a subset
```

Every test is built and run twice: plainly (`-O2`) and under ASan/UBSan
(`-fsanitize=address,undefined`, leaks detected, UBSan fatal). `run.sh` prints
`i915 contract tests PASS: ...` and exits 0 only when every check of every
variant held.

## Files

| File | Role |
| --- | --- |
| `contract.[ch]` | the check recorder every test uses |
| `mock_mmio.[ch]` | register file + forcewake handshake behind `struct i915_mmio_ops` |
| `mock_dma.[ch]` | non-identity, non-linear address producer behind `struct i915_dma_ops` |
| `mock_pci.[ch]` | 256-byte config space + MSI vector allocator behind `struct i915_pci_ops` |
| `mock_rpm.[ch]` | resume/suspend counter behind `struct i915_rpm_ops` |
| `host_kernel.[ch]` | single-threaded stand-in for spinlocks, wait queues, ticks and `kern_logf` (sync test only) |
| `host_thread.c` | `kthread_create`/`thread_start` that never run the thread (sync test only) |
| `host_unreached.c` | the real-device calls the linked sources contain; each aborts with its name if reached |
| `*_contract_test.c` | one program per contract |

| Test | Production sources |
| --- | --- |
| `mmio` | `mmio.c`, `trace.c` |
| `dma` | `dma.c`, `trace.c` |
| `pci` | `pci.c`, `trace.c` |
| `rpm` | `runtime-pm.c`, `pci.c`, `trace.c` |
| `pte` | `ggtt.c`, `ppgtt.c` (encoders only) |
| `sync` | `sync.c` (completion only), `workqueue.c`, `mmio.c`, `trace.c` |

## Host build notes

- The production files are compiled with the host compiler against the tree's
  `include/`, with the host C library preferred (`-idirafter include/libc`).
  `errno` values are therefore the host's; the tests compare symbolic names
  (`EINVAL`, `EBUSY`, ...), never numbers.
- `workqueue.c` and `host_thread.c` include `kern/thread.h`, whose
  `<sys/types.h>` clashes with the host's; `run.sh` builds those two against
  the zedBSD C library headers (`-I include/libc`).
- No production file is changed or conditionally compiled for the tests.

## Changes from the old suite (`i915-old/parity/tests/`)

Every check of the old suite was carried over against the new function unless
listed here.

- **errno**: every refusal is now a positive errno (`-62` ETIME -> `ETIMEDOUT`
  for the forcewake acknowledge timeout, `-22` -> `EINVAL`, `-16` -> `EBUSY`,
  `-19` -> `ENODEV`, `-5` -> `EIO`); `map_sgtable` failure is `!= 0` instead
  of `< 0`.
- **pci**: the vector allocator now returns `0` + the vector through a
  pointer, or a positive errno (was: the vector or `-errno`); the mock follows.
- **rpm**: `struct i915_rpm_ops` has no name field; the mock moved from the
  test into `mock_rpm.[ch]`.
- **pte**: the test-only `parity_ppgtt_pte_encode(dma, len, mask, &pte)` was
  dropped from production. Its checks now run on
  `drv_i915_gen12_ppgtt_pte_encode(dma, pat_index)` with PAT index 0, which
  produces the same `addr | PRESENT | RW` leaf. The production PPGTT encoder
  takes no length or mask and does no range or alignment check (its callers
  check with `drv_i915_dma_in_range()`), so the align/oob refusals are checked
  on the GGTT encoder only, as before. Added: the PPGTT leaf keeps an address
  above 4 GiB whole; an out-of-range GGTT encode leaves `*pte_out` untouched.
- **sync**: the old test checked `osdep/sync.c`, a single-threaded model of
  completions and work queues that was not ported. Its checks now run on the
  production `sync.c` completion and `workqueue.c` with `host_kernel.c`
  standing in for the kernel: a spinlock only counts, a sleep runs a hook (an
  interrupt arriving while the waiter sleeps) and lets one tick pass. The
  worker thread is created but never runs. Carried over:

  | Old | New |
  | --- | --- |
  | SYNC-1 complete before wait | same; also no sleep taken |
  | SYNC-2 timeout | same; done count checked via `completion.done` |
  | SYNC-3 complete mid-wait (tick callback) | the sleep hook completes on the 3rd sleep |
  | SYNC-4 N completes, N waits | same |
  | SYNC-5 complete_all + reinit | reinit only (see dropped) |
  | WQ-1 queue does not run inline | same; plus PENDING state and ring count |
  | WQ-3 cancel before run | cancel removes it, IDLE, ring empty, second cancel returns 0 |
  | WQ-5 double queue | same |
  | WQ-8 cancel_work_sync on a pending work | same; plus no sleep taken |

  Also checked: no spinlock is left held after the completion and queue calls.

### Dropped checks

| Old check | Why |
| --- | --- |
| SYNC-5 `complete_all` opens the completion permanently | production `sync.c` has no `complete_all`; the driver does not use it |
| WQ-2 flush runs the pending work | needs the worker thread, i.e. the kernel scheduler |
| WQ-3 "canceled work never ran" after a flush | the flush part needs the worker; the cancel part is kept |
| WQ-4 ordered queue runs FIFO | needs the worker to run the queue |
| WQ-6 a running work cancels a pending one | needs the worker to run a callback |
| WQ-7 a running work re-queues itself and runs twice | needs the worker to run a callback |
| WQ-8 "never ran" after a flush | the flush part needs the worker; the cancel_work_sync part is kept |
| `run_order`, `osdep_workqueue_pending`, `canceled` fields | test-only fields of the dropped model; replaced by `state`, `queued`, `drv_i915_work_pending()`, `queue->count` |

The worker-dependent behaviour (run order, flush, cancel of a running callback,
self re-queue) and `drv_i915_workqueue_destroy()` are not checked on the
host; only a kernel test with the real scheduler can check them.  The host test never destroys its queue because the destroy waits
for a worker that never ran (the queue owns no memory).

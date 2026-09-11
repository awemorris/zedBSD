# WS004 Phase 008: SMP kernel-heap integrity during boot

Last updated: 2026-08-26

Phase ID: `ws004-p008`

Status: complete; automatic QEMU gate passed in q010

Parent: [WS004](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Find and eliminate the rare amd64 SMP kernel-heap free-list corruption exposed
during the `ws004-p006` repeated USB-boot gate, without disguising the fault as
a timeout or weakening heap validation. Resume the blocked 500-boot HW-T12
gate only after the first corrupting write or invalid lifecycle transition is
identified and corrected.

## Baseline evidence

The post-USB-fix HW-T12 run reached 25 consecutive logins, then run 26 faulted
immediately after `init: started syslogd pid 3`:

```text
amd64 fault v=13 rip=FFFFFFFF:8026A0DF err=00000000 cr2=00000000:00000000
fatal: src/hal/amd64/int.c:129: unhandled amd64 fault
```

The exact address resolves to `remove_free()` in `libc/heap.c`, at the store
through `block->next_free`; the free-list link was already invalid when the
allocator tried to unlink the block. The later `hal_cpu_idle` and framebuffer
faults are concurrent panic-output fallout and are not treated as independent
root causes.

The retained run-26 disk image reached `login:` when booted again with the same
topology. The defect is therefore not persistent UFS/overlay-media corruption.
Of the first 36 attempted post-fix boots, 35 reached login without a USB or
storage failure and one hit this kernel fault. This sample does not satisfy the
500-run acceptance gate.

## Scope

- Preserve the exact q35, xHCI USB mass-storage, SMP=4, and ISA NE2000
  reproducer and classify a kernel fault separately from USB/storage failures.
- Add bounded heap invariant checks around allocation/free transitions and
  report the first damaged block and neighboring physical/free-list links.
- Correlate heap operations with CPU, allocation/free call site, size, and a
  bounded sequence number without printing every successful operation.
- Compare SMP=1 and SMP=4 and use IDE attachment as a storage-path control.
- Audit the fixed heap's split, aligned split, merge, grow, and free-list
  transitions, plus client buffer bounds and double-use lifecycles active while
  init starts services.
- Establish whether the USB URB change only perturbed timing/layout or owns the
  corrupting write. Use disposable diagnostic builds and retain exact binary
  fingerprints; do not infer causality from the fault appearing in HW-T12.

## Leading hypotheses

| Priority | Hypothesis | Required evidence |
| --- | --- | --- |
| H1 | A kernel client overwrites a neighboring heap header or uses freed storage | Heap guards identify the first changed header and the allocating/freeing call sites before `remove_free()` faults |
| H2 | A split/merge/aligned-allocation transition creates inconsistent physical and free links | A focused allocator sequence or pre/post invariant check fails while all client guards remain intact |
| H3 | Allocator serialization permits re-entry or a lock/IRQ lifecycle error on SMP | Operation records show overlapping ownership or entry while the lock is held; SMP=1 versus SMP=4 supports but does not alone prove this |
| H4 | The `ws004-p006` URB lifecycle change directly corrupts or prematurely frees memory | A correlated URB/request address overlaps the damaged block, or a controlled code-level comparison changes the first corrupting transition |

Disposition: H3 is confirmed as the q009 fault. The syslogd call graph reaches
the only unlocked allocator calls in the kernel at exactly the observed boot
stage. H2 also exposed a deterministic aligned-allocation defect, but that
function was absent from the q009 kernel's live call graph and is not assigned
as the observed root cause. H1 and H4 have no supporting corruption evidence.

## Ordered work

- [x] Extend the boot-stress classifier and preserve the original run-26
      symbolization and exact QEMU/build metadata.
- [x] Add a focused host allocator fixture for split, aligned split, merge,
      realloc, grow, invalid free, and randomized invariant validation.
- [x] Establish the first invalid transition without runtime heap guards: the
      linked kernel and syslogd call graph prove an unlocked libc allocation on
      the shared heap, and the focused test separately reproduces aligned-split
      header corruption. Additional perturbing history instrumentation was not
      needed.
- [x] Reproduce or characterize the fault with sequential SMP=1/SMP=4 USB and
      IDE controls; preserve every kernel-failure image and log.
- [x] Correct only the layer proved to own the corruption and add a focused
      regression for the demonstrated interleaving or bounds error.
- [x] Run focused tests, `make -j16`, and `git diff --check`; do not use
      `.internal/` or `make check`.
- [x] Restart HW-T12 from run 1 after the correction. Return the 500-run
      result to `ws004-p006`, including zero kernel, USB, and storage failures.

## Completion conditions

- The first invalid heap transition is explained by correlated evidence, not
  merely made rarer by delays, logging, CPU pinning, or retries.
- The owning fix has a focused regression which fails for the old behavior and
  passes for the correction.
- Kernel heap validation remains enabled and no invalid free is silently
  accepted.
- The exact SMP=4 q35/xHCI HW-T12 topology completes 500 sequential pristine
  image boots with zero kernel, USB, storage, or evidence-classification
  failures, allowing `ws004-p006` to finish its remaining acceptance work.
- Focused tests, `make -j16`, and `git diff --check` pass without using
  `make check`.

## Resume condition

Completed in q010. The revised 500-run automatic gate passed, with one extra
corroborating pass; detailed manual acceptance remains a user-operated
follow-up. See [q010 evidence](../tests/q010-hwt12-evidence.md).

## Result

The observed fault was a shared-lock-domain violation. syslogd reads the large
`kern.msgbuf` immediately after startup; the syscall's overflow buffer used
libc `malloc/free` on `kernel_heap` through weak no-op hooks while every
`kern_malloc/free` user updated that same free list under an SMP lock. The
syscall now uses `kern_malloc/free`, and strong kernel libc hooks protect future
compatibility calls with the same IRQ-safe lock.

The allocator fixture also found and corrected an independent aligned-prefix
underflow. The corrected allocator passed 100,000 randomized operations and
eight concurrent workers with 20,000 operations each. SMP=1 USB, SMP=4 USB,
and SMP=4 PC/IDE controls each passed 10/10. The final q35/xHCI/SMP=4 run passed
501/501 with zero kernel, USB, storage, or harness failure; the first 500 form
the approved automatic gate.

# μITRON-compatible real-time domain

Status: planned; design discussion is manually blocked and no implementation
claim is made

Last reviewed: 2026-08-27

Owner: [WS015](../../plan/ws015-muitron-rt/ws.md)

## Purpose

This note records the current design direction for combining a
μITRON-compatible real-time domain with zedBSD's UNIX/POSIX environment. It
separates decisions already made from questions that must be settled before an
implementation Phase can be authorized.

The machine remains physically SMP. An optional build may reserve selected
cores for RT execution and use the remaining cores for ordinary zedBSD/POSIX
work. This is an AMP-like division of responsibility, but the mixed POSIX/RT
architecture, protection model, service bridge, and failure contract are
zedBSD designs. The published μITRON multicore guideline is only a limited
reference for static processor roles; it does not define this complete model.

## Confirmed design direction

### User-mode RT execution

RT applications run as resident user-mode ELF images by default. Kernel-mode
RT application tasks are not the normal compatibility or deployment model.
One admitted RT image may contain multiple μITRON tasks in a shared RT address
space, allowing legacy tasks to continue using globals, fixed memory pools,
and pointer-based intra-image messages without turning every task into a
separate POSIX process.

Admission prepares all resources before an RT task is dispatched. Code, data,
BSS, TLS, stacks, page tables, μITRON objects, communication buffers, and the
small emergency path are prefaulted and pinned. Demand paging, swap, reclaim,
unbounded stack growth, dynamic loading, and post-admission mappings are not
part of the RT execution contract.

### MMIO and privileged operations

An RT image may receive explicit mappings for only the MMIO regions required
by its admitted device role. The grant must be established before dispatch and
must not imply arbitrary physical-memory access. Architecture operations that
cannot safely be exposed through an MMIO page, such as x86 port I/O, interrupt
controller programming, timer ownership, MMU/IOMMU changes, and reset, remain
behind bounded kernel or HAL entry points.

The kernel retains the RT scheduler, interrupt top halves, timers, memory
protection, watchdog/reset path, and any minimal driver shim that must execute
privileged. Most control logic remains in the user-mode RT image. DMA
ownership, interrupt delivery, allowed MMIO ranges, and the exact admission
manifest remain design questions.

### Compatibility and migration direction

The initial target is source and behavioral compatibility with a declared
μITRON profile, not undocumented binary compatibility with existing kernels.
The design should make gradual migration of existing assets practical:

1. link existing tasks, global state, and fixed pools into one RT ELF image;
2. translate static object and startup configuration through a host-side or
   build-time configuration mechanism;
3. replace board-support and privileged register operations with explicit MMIO
   grants or bounded HAL shims;
4. proxy filesystem, network, and other non-real-time work to POSIX cores; and
5. split protection domains later only where the application needs it.

This preserves the common μITRON programming model more closely than mapping
each task to an independently protected POSIX-style process.

## Messages between RT and POSIX

The μITRON mailbox model passes a pointer to a message packet. That is usable
inside one admitted RT address space, where sender and receiver share the
packet representation and ownership rules. A raw RT virtual address must not
cross directly into an unrelated POSIX process.

The RT/POSIX bridge therefore needs a separate bounded protocol. The current
direction is to use preallocated requests and replies with an explicit request
identifier, operation, payload length, result, timeout behavior, and ownership
state. A POSIX-side broker validates or copies the request, performs the
ordinary service, and returns a reply. Exact use of mailboxes, data queues,
message buffers, fixed pools, and shared slots is not yet frozen.

## Filesystem access

Filesystem access is not part of the μITRON kernel API contract currently
proposed for WS015. File operations are therefore zedBSD extensions implemented
by a broker on POSIX cores rather than by running the VFS, block I/O, or
filesystem workers on an RT core.

Two caller interfaces remain under discussion:

- a compatibility wrapper that submits a request and blocks only the calling
  RT task until its reply arrives; and
- a native asynchronous request/reply interface that lets the RT application
  continue other work and receive completion through an RT object.

The blocking wrapper is likely useful for migrating existing code, while the
asynchronous form exposes the timing boundary more honestly. In either form,
POSIX filesystem completion is not a hard-real-time guarantee. A hard-critical
task must not depend on the broker completing within its deadline unless a
specific bounded service is later designed and demonstrated.

## Hard-real-time and failure boundary

A hard-real-time claim will apply only to a named board, firmware and power
configuration, RT core and interrupt assignment, admitted image, and workload
envelope. QEMU can provide functional and fault-injection evidence but cannot
establish that physical timing bound.

POSIX failure handling is deliberately limited. The RT domain may receive an
explicit failure notification or detect missed heartbeats, run one fully
resident emergency action, and request watchdog or platform reset. The design
does not promise survival from arbitrary corruption of shared kernel state,
the RT core, memory fabric, firmware, power, or reset hardware.

## Unresolved discussion

The following decisions block implementation:

1. the exact μITRON edition, profile, service-call matrix, and documented
   deviations;
2. static configuration, task/handler startup, linker, runtime, and legacy BSP
   migration contracts;
3. RT CPU masks, scheduler priorities, clocks, timers, interrupt ownership,
   and bounded shared-kernel paths;
4. ELF form, admission authority, memory budgets, fault containment, and the
   MMIO/IRQ/DMA grant manifest;
5. the intra-RT object model and the copied or shared-slot RT/POSIX protocol,
   including ownership, capacity, cancellation, restart, and stale replies;
6. blocking compatibility and asynchronous filesystem APIs, request pools,
   file-handle ownership, and timeout semantics;
7. heartbeat, emergency action, watchdog, reset, and excluded failure cases;
8. the first physical target and its measurable microsecond guarantee; and
9. public naming, conformance language, attribution, headers, tooling, and
   licensing review.

WS015 and its discussion Phase remain under manual hold until the user
explicitly resumes these topics. No Queue may infer answers to them from this
note.

## References

- [WS015 plan](../../plan/ws015-muitron-rt/ws.md)
- [WS015 architecture discussion Phase](../../plan/ws015-muitron-rt/phase001-architecture-discussion/phase.md)
- [TRON Forum specifications](https://www.tron.org/specifications/)
- [μITRON 4.0 multicore processor extension guideline](https://www.tron.org/ja/wp-content/themes/dp-magjam/pdf/specifications/ja/WG024-W030-01.00.00.pdf)

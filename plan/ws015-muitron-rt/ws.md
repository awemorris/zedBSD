# WS015: μITRON asymmetric real-time domain

Last updated: 2026-08-27

WSID: `ws015`

Status: Blocked by manual hold `MB-007`; architecture/API discussion required

Parent: [master plan](../master.md)

Last verified Phase: none

Resume point: after explicit release of `MB-007`, continue discussion and
freeze `ws015-p001`; no implementation Phase or Queue item exists.

Shared reviews: [WS015 review index](tests/README.md)

Architecture note:
[μITRON-compatible real-time domain](../../docs/architecture/muitron-rt-domain.md)

## Goals

- Independently implement a declared μITRON 4.0 API/profile as a zedBSD UAPI.
- Allow an enabled build to reserve selected physical SMP cores as an
  asymmetric RT domain on which only admitted RT tasks and required RT kernel
  paths execute.
- Admit resident RT ELF programs whose executable state cannot demand-fault,
  reclaim, or swap while executing on an RT core.
- Connect RT and POSIX domains through bounded message-passing interfaces, so
  existing μITRON-oriented control code can request non-real-time POSIX work
  without executing POSIX services on an RT core.
- Establish an explicit, board-specific hard-real-time contract expressed as a
  measured and justified maximum latency under a fixed hardware, firmware,
  interrupt, and workload configuration.
- Permit a still-functioning RT domain to run one pre-admitted emergency action
  and request system reboot when POSIX-domain failure is detected, without
  promising survival from failures that corrupt shared kernel state or RT
  hardware.

## Objective

Combine zedBSD's UNIX/POSIX environment with a deliberately isolated
μITRON-oriented control domain. The machine remains physically SMP, but an
RT-enabled configuration statically partitions cores and responsibilities in
an AMP-like model: normal POSIX processes remain on POSIX cores, while
dedicated RT cores run only admitted RT work, RT-owned interrupts, and the
minimum kernel mechanisms required by the selected μITRON API profile.

The workstream initially targets source and behavioral compatibility with a
declared subset/profile of the μITRON 4.0 specification. It must not claim
general μITRON conformance or a universal hard-real-time bound until the exact
profile, deviations, test suite, target board, and measured contract are
published.

The official μITRON 4.0 multicore extension guideline is only a limited
reference for static core roles. It does not specify zedBSD's mixed POSIX/RT
protection, service-bridge, or failure model. zedBSD will independently design
and implement those mechanisms while treating the published μITRON
specification as the selected API and behavior reference.

## Scope

- feature-gated build and boot configuration for one or more RT cores;
- admission of RT ELF images and allocation of their task, stack, TLS, data,
  BSS, UAPI, page-table, and communication resources before dispatch;
- page pinning, prefault verification, swap/reclaim exclusion, and a
  fault-free execution contract after admission;
- fixed-priority RT scheduling, local timer behavior, CPU/dispatch control,
  RT-owned interrupt routing, and bounded synchronization primitives;
- a declared μITRON 4.0 API/profile, types, error values, object identifiers,
  lifecycle rules, static/dynamic configuration boundary, and compatibility
  deviations;
- kernel RT primitives wrapped by a public μITRON-shaped UAPI without exposing
  kernel pointers or ordinary POSIX syscalls to the RT core;
- intra-RT synchronization and communication plus a separately specified
  RT/POSIX mailbox bridge with explicit copy, ownership, queue, timeout, and
  backpressure semantics;
- a POSIX-side broker that performs requested non-RT services and returns
  bounded protocol responses without making their completion hard-real-time;
- POSIX-failure notification, heartbeat-loss detection, one resident emergency
  path, and architecture watchdog/reset handoff;
- functional tests in QEMU and timing/isolation evidence on a named physical
  target with a fixed configuration;
- licensing, naming, compatibility, and public documentation handoff.

## Non-goals

- running arbitrary POSIX syscalls, signals, filesystems, networking, dynamic
  loading, or ordinary kernel workers on an RT core;
- promising that POSIX files, sockets, or broker operations complete within a
  hard-real-time deadline;
- preserving RT operation after arbitrary memory corruption, RT-core failure,
  power loss, shared interconnect failure, non-maskable firmware interference,
  or hardware reset;
- making the Dell Latitude 5320 the timing-contract target before its firmware
  interference and interrupt topology are shown to be controllable;
- claiming safety certification or general WCET proof from latency sampling;
- copying an existing μITRON kernel implementation into the zedBSD base
  system.

## Dependencies

- [WS001](../ws001-posix/ws.md) owns the ordinary POSIX boundary and records
  any interaction or incompatibility debt; RT APIs remain a distinct profile.
- [WS003](../ws003-bringup/ws.md) and
  [WS004](../ws004-hardware/ws.md) provide CPU topology, SMP startup, local
  timers, interrupt routing, MSI, reset/watchdog, and target-board evidence.
- [WS009](../ws009-documentation/ws.md) publishes the final API, restrictions,
  target-specific guarantees, and compatibility declaration.
- The current kernel has per-CPU scheduler queues, SMP startup, IRQ affinity,
  and VM residency/pinning foundations, but none is presently an RT isolation
  or bounded-latency guarantee.

## Normative references for discussion

- [TRON Forum specifications](https://www.tron.org/specifications/) lists the
  μITRON 4.0 specification, Japanese Ver. 4.03.03 and English Ver. 4.03.00.
- [μITRON 4.0 multicore processor extension guideline](https://www.tron.org/ja/wp-content/themes/dp-magjam/pdf/specifications/ja/WG024-W030-01.00.00.pdf)
  is the reference for the AMP model; zedBSD's mixed POSIX/RT arrangement and
  UAPI remain zedBSD-specific extensions.
- [TRON Forum naming guidance](https://www.tron.org/ja/tron-project/itron/i_products/itronmanual/)
  records the specification ownership and terminology that later public
  documentation must respect.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws015-p001` | [Architecture and compatibility discussion](phase001-architecture-discussion/phase.md) | Blocked by manual hold `MB-007` | Freeze the μITRON profile, RT/POSIX partition, resident-execution contract, failure boundary, timing target, and later Phase map |

No implementation Phase is defined until `ws015-p001` is complete.

## Confirmed product direction

- The feature is build-time optional; a disabled build retains the ordinary
  zedBSD SMP/POSIX model.
- RT cores have static roles. Normal scheduler load balancing must not migrate
  POSIX work into them.
- An RT program is fully prepared by non-RT code and then admitted. After
  admission it cannot depend on demand paging, swap, reclaim, dynamic linking,
  or unbounded POSIX services.
- RT application code runs in user mode by default. One resident RT ELF may
  contain multiple μITRON tasks in a shared RT address space, preserving
  globals, fixed pools, and intra-domain pointer-based messages.
- Required MMIO is granted as explicit, pre-admission mappings. Interrupt
  control, timers, MMU/IOMMU operations, reset, and other privileged operations
  remain bounded kernel/HAL responsibilities; arbitrary physical access is not
  granted.
- RT and POSIX worlds communicate by message passing. POSIX work is delegated
  to a POSIX-side broker rather than executed on an RT core.
- Filesystem requests use that broker. A blocking compatibility wrapper and a
  native asynchronous request/reply API both remain candidates, and broker
  completion is outside the hard-real-time guarantee.
- A hard-real-time guarantee is specific to one declared board/configuration
  and one declared workload envelope; QEMU is only a functional test target.
- POSIX failure handling is intentionally narrow: notify when possible, also
  detect heartbeat loss, run a small resident emergency routine, and reboot.

## WS completion direction

The planning-stage WS is manually blocked while p001 remains under discussion.
After release, it may pause after p001 has produced a coherent architecture
and bounded implementation Phase decomposition. Full WS completion will later
require the selected μITRON profile and deviations to be documented, functional
and negative isolation tests to pass, at least one named target to meet its
published latency contract, and crash/emergency behavior to match the declared
limited guarantee.

## Reconsideration boundaries

Reconsider before implementation if the selected μITRON compatibility profile
requires an execution/configuration model incompatible with the resident ELF
boundary; if cross-domain mailbox semantics require unsafe shared pointers; if
ordinary kernel locks, TLB shootdowns, allocators, or interrupts can enter the
RT core without a bounded path; if hardware cannot route the required
interrupts and timer independently; or if the requested POSIX-crash guarantee
would require fault containment unavailable in a shared kernel/address space.

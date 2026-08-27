# WS015 Phase 001: μITRON RT architecture and compatibility discussion

Last updated: 2026-08-27

WSID: `ws015`

Phase ID: `p001`

Combined ID: `ws015-p001`

Status: Blocked by manual hold `MB-007`; discussion required

Parent: [WS015](../ws.md)

Reviews: [WS015 review index](../tests/README.md)

Architecture note:
[μITRON-compatible real-time domain](../../../docs/architecture/muitron-rt-domain.md)

## Objective

Turn the asymmetric μITRON/zedBSD concept into a reviewable contract before
selecting any code work. Freeze what is compatible, what is zedBSD-specific,
what may execute on an RT core, how resident ELF admission works, how the two
worlds communicate, what survives a POSIX failure, and how a board-specific
hard-real-time bound will be demonstrated.

## Baseline

- The physical machine is SMP; the requested logical model statically reserves
  selected cores for RT work and leaves the remaining cores to zedBSD/POSIX.
- zedBSD already has SMP startup, per-CPU scheduler queues, interrupt affinity,
  page-residency/pinning mechanisms, and platform reset paths. They are useful
  foundations but currently provide no isolation or latency guarantee.
- The TRON Forum publishes μITRON 4.0 Ver. 4.03 and a separate μITRON 4.0/AMP
  multicore guideline. The latter's static roles and non-migrating AMP model
  align with the requested direction, but it does not by itself specify
  zedBSD's mixed POSIX/RT failure or UAPI boundary.
- A shared kernel cannot promise that an RT core survives arbitrary POSIX-side
  memory corruption or shared-lock corruption. The initial recovery contract
  must therefore be a bounded fail-stop case, not fault containment equivalent
  to a separate safety processor.

## Scope

### Compatibility contract

- Select the exact μITRON 4.0 edition and initial profile, likely choosing
  among the specification's standard/basic surface or a documented zedBSD
  subset rather than claiming the entire specification by implication.
- Inventory public service calls, types, constants, error values, object IDs,
  task-entry conventions, handler context, dispatch/CPU-lock states, and time
  units.
- Decide how existing assets provide static object configuration and startup
  declarations, including whether a host configuration generator is needed.
- Distinguish source compatibility, behavioral compatibility, supported
  profile, extensions, and explicit deviations in names and documentation.

### Core and interrupt partition

- Define build options, boot parameters, CPU masks, minimum CPU count, invalid
  configuration behavior, and whether more than one RT core is initially
  allowed.
- Define a separate RT scheduler and idle path; prohibit POSIX tasks, normal
  load balancing, work queues, reclaim, and unbounded deferred work on RT
  cores.
- Assign local timer, RT-owned device interrupts, inter-processor messages,
  interrupt priorities, affinity, nesting, and non-maskable/firmware caveats.
- Audit all cross-domain locks, allocations, cache/TLB maintenance, logging,
  tracing, panic, suspend, hotplug, and stop-the-world paths for boundedness or
  exclusion.

### Resident ELF admission

- Define a two-stage lifecycle: an ordinary POSIX loader verifies and prepares
  the image, then an admission operation transfers it to the RT domain.
- Define supported ELF form, initially considering static `ET_EXEC` or static
  PIE only, and decide whether shared libraries, interpreters, TLS, and
  relocations are excluded.
- Prefault and pin every executable/data/BSS/TLS/stack/UAPI/page-table and
  communication page required after admission; reserve object-control and
  emergency resources as well.
- Ban or bound demand faults, copy-on-write, swapping, reclaim, stack growth,
  dynamic loading, runtime allocation, and post-admission mapping changes.
- Define the result of a prohibited access: deterministic RT-task fault and
  containment, not an attempt to service a page fault on the RT core.

### μITRON UAPI and message bridge

- Define the trap/call ABI and the split between public μITRON-shaped wrappers,
  zedBSD UAPI, kernel RT primitives, and HAL mechanisms.
- Define object namespaces and prevent public APIs from exposing kernel
  addresses.
- Preserve selected μITRON mailbox semantics inside the RT domain where safe,
  while separately defining an RT/POSIX bridge. A raw message-packet pointer
  cannot silently become a durable cross-domain ownership contract.
- Freeze copied versus shared payloads, fixed maximum sizes, alignment,
  ownership transfer, queue capacity, priority/FIFO rules, timeouts,
  cancellation, POSIX broker restart, stale replies, and backpressure.
- State explicitly that completion of delegated POSIX file/network/service work
  is not part of the RT deadline unless a later bounded service is separately
  designed and proven.

### Failure and reboot contract

- Define POSIX health as both an explicit crash notification and an RT-observed
  heartbeat deadline; notification alone is insufficient when the failing core
  cannot send it.
- Pre-register and pin one minimal emergency state machine and all resources it
  needs. It may place hardware in a declared safe state, emit bounded evidence,
  and request watchdog/platform reset without POSIX scheduling or allocation.
- Enumerate supported fail-stop cases and excluded failures, including shared
  memory corruption, poisoned locks, RT-core failure, SMI/NMI/firmware delay,
  interconnect failure, power loss, and reset already in progress.
- Decide which architecture watchdog/reset primitive the RT core owns and what
  happens if reset does not complete.

### Timing and evidence contract

- Select a physical board, CPU/core mask, timer, RT device/interrupt, firmware
  settings, power-state policy, cache/memory configuration, and watchdog.
- Define the workload envelope, interference loads on POSIX cores, warm-up,
  run duration, trace overhead, clock calibration, maximum-latency metric, and
  failure threshold.
- Separate functional conformance, stress/negative isolation, measured
  worst-observed latency, and a justified guaranteed bound. Percentiles alone
  are not a hard-real-time guarantee.
- Use QEMU for deterministic functional and fault-injection tests only; physical
  hardware owns timing acceptance.

## Non-goals

- implementing the RT scheduler, loader, UAPI, or broker in this Phase;
- selecting an API merely by copying every published service-call name;
- treating ordinary `mlock`-like residency as sufficient RT admission;
- claiming safety certification, universal microsecond latency, or independent
  fault containment from a shared kernel;
- adding a current Queue entry before this discussion completes.

## Confirmed decisions

- RT application code runs in user mode by default. Kernel mode is reserved
  for the scheduler, IRQ/timer/MMU/IOMMU/reset mechanisms and bounded driver or
  HAL shims that require privilege.
- One admitted resident RT ELF may contain multiple μITRON tasks in a shared
  RT address space. This preserves globals, fixed pools, and pointer-based
  intra-domain messages without modeling every task as a POSIX process.
- The complete admitted domain, including stacks, page tables, communication
  buffers, object state, and emergency resources, is prefaulted and pinned.
- Necessary MMIO pages are mapped explicitly before dispatch. Admission does
  not grant arbitrary physical-memory access; IRQ, DMA, IOMMU, architecture
  port-I/O, and mapping policy still require a frozen contract.
- POSIX filesystem and similar non-real-time work is delegated to a broker on
  POSIX cores. Its completion time is not included in the hard-real-time
  guarantee.
- Existing μITRON assets should be migratable as one image with their shared
  state and static object model, using shims for privileged BSP operations and
  POSIX service requests. The target is a declared source/behavioral profile,
  not implied binary compatibility.
- The official multicore guideline is a limited reference for static processor
  roles, not a complete specification of zedBSD's mixed POSIX/RT design.

## Open decisions requiring discussion

1. Exact μITRON 4.0 edition/profile and acceptable deviations.
2. Compatibility level for legacy assets, especially static configuration and
   task/handler startup rather than only service-call spelling.
3. One initial RT core or multiple; build option and boot-time CPU-mask syntax.
4. Exact RT ELF format, linker/runtime contract, memory budget, admission
   authority, fault containment, and the MMIO/IRQ/DMA grant manifest. User-mode
   execution and explicit MMIO mapping are already fixed.
5. RT scheduler, priority count, time base, timer, interrupt ownership, and
   bounded kernel paths.
6. Intra-RT mailbox API versus copied/shared-slot RT/POSIX bridge semantics,
   broker protocol, ownership, capacity, cancellation, and restart behavior.
7. Filesystem compatibility wrapper versus native asynchronous API, including
   request/reply pools, handle ownership, timeout, cancellation, and the
   boundary between soft and hard-real-time callers.
8. POSIX heartbeat source, failure threshold, emergency action API, watchdog,
   and excluded failure classes.
9. First physical target and the specific microsecond guarantee/workload to be
   attempted.
10. μITRON naming, compatibility claim, specification attribution, headers,
   tooling, and independent-implementation documentation.

## Work packages

- [ ] Freeze the normative specification edition/profile and compatibility
      matrix.
- [ ] Freeze the CPU, scheduler, timer, interrupt, and shared-kernel isolation
      model.
- [ ] Freeze the RT ELF admission, resident memory, and prohibited-operation
      contracts.
- [ ] Freeze the μITRON UAPI layering and RT/POSIX message/broker protocol.
- [ ] Freeze the fail-stop notification/heartbeat/emergency/reboot guarantee
      and exclusions.
- [ ] Freeze the first board-specific latency contract and evidence method.
- [ ] Allocate ownership across WS001, WS003, WS004, WS009, and WS015.
- [ ] Split later work into bounded specification, HAL partition, RT kernel,
      loader/admission, UAPI/lib, broker, recovery, conformance, timing, and
      documentation Phases.

## Acceptance

Every case in the [WS015 review index](../tests/README.md) has an explicit
decision, owner, failure semantics, and later verification environment. The
resulting Phase map must separate QEMU-functional work from physical timing
claims and must not require an unresolved human decision inside an
implementation Queue.

This Phase makes no runtime, μITRON-conformance, or hard-real-time claim.

## Actual results and evidence

The product direction and initial risk boundaries are recorded in this plan
and the linked architecture note. User-mode resident RT ELF execution,
explicit MMIO grants, and POSIX-side filesystem brokering are fixed directions.
The ten listed design decisions remain unresolved and no source implementation
has been authorized.

## Interruption / resumption

This Phase is manually blocked by `MB-007`. Resume only after the user
explicitly reopens the μITRON architecture/API discussion, beginning with
decision 1, the exact profile and legacy-source compatibility level. Until
then, do not add it to a Queue or extract implementation Phases from
assumptions.

## Remaining debt and handoff

All code, build options, headers, configuration tooling, test fixtures, target
selection, physical measurements, compatibility reports, and public
documentation remain future Phases.

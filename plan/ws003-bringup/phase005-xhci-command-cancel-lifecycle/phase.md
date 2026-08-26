# WS003 Phase 005: xHCI command and cancellation lifecycle

Last updated: 2026-08-26

WSID: `ws003`

Phase ID: `p005`

Combined ID: `ws003-p005`

Status: Complete (`q013`)

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md)

Evidence: [Latitude xHCI evidence](../tests/latitude-xhci-evidence.md)

## Objective

Make xHCI command completion, transfer cancellation, slot teardown, and DMA
ownership fail closed before further physical enumeration work. A request,
bounce buffer, transfer ring, device context, and slot remain durably owned
until the controller has acknowledged the state transition which makes each
resource safe to release.

## Baseline and confirmed boundary

The q012 Latitude run reached a timed-out EP0 request and then reported:

```text
xhci: command 15 failed, completion=19
xhci: endpoint 1 stop failed during cancel (5); retaining DMA buffer
```

Command 15 is Stop Endpoint and completion 19 is Context State Error. Code
review found two independent P1 lifecycle defects:

- cancellation clears `c->active` and the URB HCD pointer before Stop Endpoint;
  a failed command then leaves the request and bounce DMA without a durable
  owner;
- Set TR Dequeue and Disable Slot failures do not prevent later request,
  transfer-ring, or context release, so live DMA can reference freed memory.

Command Completion Events also lack command-TRB pointer association and may be
consumed by the IRQ path while the synchronous command waiter is polling.

## Scope

- Associate every Command Completion Event with its submitted Command TRB and
  expose the raw completion code to the state machine.
- Route IRQ- and polling-consumed command events through one ownership path.
- Track xHCI endpoint state and select Stop Endpoint, Reset Endpoint, and Set
  TR Dequeue only from valid states.
- Keep the active request and URB HCD pointer reachable until cancellation has
  safely removed the TD from controller ownership.
- Prevent device, slot, ring, context, and DMA release after failed recovery or
  failed Disable Slot.
- Absorb the bounded interval between terminal-status publication and release
  of completion/HCD ownership instead of misclassifying it as teardown failure.
- Retain an unrecoverable device/controller in a bounded quarantine which
  blocks re-enumeration and retrying DMA work but permits later safe teardown.
- Add deterministic host fault injection and a QEMU auxiliary-device
  remove/re-add lifecycle gate.
- Feed the one later BR-T34 result into this Phase as normal-path physical
  evidence; do not request a separate p005 hardware run.

## Non-goals

- Control Setup/Data/Status TRB formatting, port reset, and EP0 packet-size
  correction; those are `ws003-p004`.
- General multi-request scheduling, USB hubs, HID, storage protocol, VFS, or
  controller suspend/resume.
- Declaring failure-path safety from a successful boot alone. BR-T28/BR-T29
  fault injection is the primary evidence.

## Dependencies

- The p003 checked controller-quiesce and PCI quarantine contract.
- Existing USB URB publication, xHCI model, PCI lifecycle, and heap fixtures.
- `ws003-p004` consumes this safe command/cancellation foundation afterward in
  the same q013 Queue.

## Fixed decisions

- Command serialization is not sufficient association. The completion-event
  Command TRB Pointer must match the exact submitted TRB before the waiter
  accepts its completion code.
- IRQ and polling paths may observe events, but only one path publishes a
  matched command completion; stale or unrelated events cannot satisfy the
  current command.
- Endpoint state is read from the controller-owned output context after the
  relevant Transfer/Command Event ordering point; software does not predict a
  state transition. Running, Halted, Stopped, Error, and Disabled are distinct
  recovery inputs.
- A Running endpoint is stopped before dequeue movement. A Halted endpoint is
  reset before Set TR Dequeue. Stopped/Error paths use only the transitions
  permitted by the xHCI state machine; completion 19 causes state
  re-evaluation, not blind looping.
- The request, bounce buffer, and URB HCD reference remain owned until Set TR
  Dequeue succeeds or the whole controller is proven quiescent.
- Disable Slot success, or whole-controller quiescence, precedes transfer-ring,
  input/output-context, DCBAA entry, and device-state release.
- A failed checked device teardown remains visible to USB core and blocks
  freeing/re-enumerating that port. Legacy UHCI/EHCI behavior is not broadened
  unless the new optional checked boundary is used.
- No q013 physical boot occurs until p004--p009 pass every automatic gate.
  BR-T34 is shared by all six Phases.

## Expected files and subsystems

- `drivers/pci-xhci.c`
- a shared xHCI command/recovery helper under `include/drivers/`
- `drivers/usb.c` and `include/drivers/usb.h` for an optional checked device
  teardown/quarantine boundary
- `plan/ws003-bringup/tests/xhci-cancel-command-test.c`
- `plan/ws003-bringup/tests/xhci-hotplug-lifecycle.sh`
- applicable WS004 USB/xHCI fixtures

## Ordered work packages

- [x] Add BR-T28 pure state/ownership fixtures covering Running, Halted,
      Stopped, Error, Disabled, completion 19, Reset Endpoint, and Set TR
      Dequeue failure.
- [x] Preserve request, URB, and DMA ownership across every failed cancel and
      recovery operation; publish terminal cancellation only after safe
      dequeue removal.
- [x] Add an optional checked device teardown boundary and require Disable Slot
      success before freeing HCD or USB-core state.
- [x] Wait boundedly for an in-progress terminal completion/HCD publication
      window before endpoint quiesce and Disable Slot.
- [x] Match Command Completion Events by Command TRB Pointer and route IRQ/poll
      interleavings through one pending-completion record.
- [x] Add BR-T29 host cases for stale/unrelated/matched command events and
      exactly-once publication.
- [x] Add BR-T29 QEMU hotplug lifecycle using IDE root plus an auxiliary xHCI
      USB storage device; remove, re-add, and re-enumerate without leaked slots,
      live DMA, timeout, or panic.
- [x] Run BR-T26, applicable URB/xHCI/PCI/heap regressions, `make -j16`, and
      `git diff --check`. Do not use `make check` or `.internal/`.
- [x] Hand the passing safe lifecycle to p004 without requesting hardware.
- [x] After the shared BR-T34 run, record its command/cancellation observations
      here even though fault-path completion rests on BR-T28/BR-T29.

## Acceptance cases

- `BR-T28`: production-shared state logic and fault injection prove no
  request, DMA buffer, ring, context, DCBAA entry, or slot is released while
  the controller can still access it; recovery is retryable and bounded.
- `BR-T29`: Command Completion pointer/IRQ/poll fixtures pass, and QEMU
  auxiliary USB remove/re-add completes without stale command consumption,
  timeout, live-DMA release, or panic.
- `BR-T26` and all applicable WS004 USB/xHCI/PCI/heap regressions pass.
- The shared `BR-T34` run contributes one normal-path physical observation;
  it does not replace BR-T28/BR-T29 failure injection.

## Completion conditions

- Command events are matched and published exactly once.
- Cancellation and device teardown have durable, retryable ownership through
  every injected command failure.
- No Disable Slot or Set TR Dequeue failure frees controller-visible memory.
- BR-T26, BR-T28, BR-T29, relevant regressions, build, and diff checks pass.
- The single shared BR-T34 result is fed back before q013 closes.

## Actual results and evidence

Execution was authorized in q013 on 2026-08-26. The implementation now matches
Command Completion Events to the exact submitted Command TRB, serializes IRQ
and polling publication, retains controller-visible request/DMA resources
until a checked state transition, recovers endpoint state before reuse, waits
for terminal/HCD publication, and requires checked endpoint/slot quiescence
before release.

BR-T26, BR-T28, and BR-T29 passed, including the QEMU auxiliary xHCI storage
remove/re-add gate with a live IDE root. Applicable USB URB, xHCI, PCI, DMA,
heap, and USB-storage regressions passed. `make -j16`, `git diff --check`, the
legacy-BIOS xHCI USB-root boot, and BR-T24 OVMF USB-root boots at 4, 8, and
16 GiB also passed.

The one shared BR-T34 normal-path run then configured three physical USB
devices, registered the SuperSpeed medium as `sda`, and entered BOT/SCSI I/O
without a command-association, cancellation, endpoint-state, retained-DMA, or
Disable-Slot failure. Together with BR-T28/BR-T29 fault injection, that
observation completes this Phase. The later unsupported SCSI opcode is owned
by `ws003-p010` and does not reopen xHCI command lifecycle.

## Interruption / resumption

No p005 work remains. The generic lifecycle debt below stays deferred; U3
resumes in `ws003-p010` without a separate p005 hardware run.

## Remaining debt and handoff

- Generic USB topology readers and lookup/dump iterators still expose raw
  bus/device/interface pointers without a reader reference contract. They
  require separate synchronization before concurrent unregister/finalize is a
  supported non-boot path.
- The generic USB driver registry, probe, and unregister paths still need one
  synchronization/lifetime contract, including detaching already-bound
  interfaces before a driver object can disappear.
- A retained request/device/controller has no automatic reaper which retries
  teardown or rescans a port after a late URB/reference drain. It remains
  fail-closed and quarantined until a later port event or future administrative
  recovery path supplies that retry.
- General multi-request queues, hub concurrency, and future IOMMU mapping
  lifetime remain separate common USB/DMA work. None is claimed complete by
  the q013 USB mass-storage boot-path gates.

## Reference

- [Intel xHCI Requirements Specification 1.2b](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf), especially sections 4.6.9, 4.8.3, 4.11, 6.4.2.3, and 6.4.2.4.

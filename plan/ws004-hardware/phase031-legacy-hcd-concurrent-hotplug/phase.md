# WS004 Phase 031: legacy HCD concurrent scheduling and root-port lifecycle

Last updated: 2026-08-31

Phase ID: `ws004-p031`

Work item: `HW-27`

Primary test: `HW-T25`

Status: complete (`q047`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Make the UHCI and EHCI runtime suitable for USB HID without weakening the
checked USB ownership and DMA-retirement contracts.  Control, interrupt, and
bulk requests on independent endpoints and devices must make progress at the
same time, a persistent NAKing interrupt-IN endpoint must not starve USB
storage, and root-port detach/reinsert must be processed after initial boot.

This Phase supplies the legacy-HCD concurrency and hotplug prerequisite for
[`ws006-p008`](../../ws006-input/phase008-usb-hid-evdev/phase.md).  It does not
implement the production USB HID class driver, evdev publication, HID report
decoding, endpoint-STALL recovery, or device reset.

## Dependencies and residual boundaries

- `ws004-p009`: checked IRQ removal, controller halt, PCI bus-master disable,
  and DMA-resource retention.
- `ws004-p010`: retained USB configuration/interface/endpoint function model.
- `ws004-p011`: exact HCD completion ownership, callback-aware URB drain, and
  `DRV_USB_HCD_CAP_CONCURRENT_URBS` semantics.
- `ws004-p015`: interface admission, endpoint-zero serialization, binding
  teardown, and the public enqueue/dequeue contract.
- `ws004-p016`: controller-observed UHCI FRNUM and EHCI Async Advance request
  retirement, including fail-closed DMA retention.

The following work remains outside this Phase:

- The general reset and endpoint-STALL contract required by `ws006-p008` is a
  separate WS004 Phase.  `drv_usb_device_reset()` currently returns `ENOTSUP`.
- Legacy short-IN/control-status semantics and exact EHCI halted-qTD error
  classification remain `BUG-003` in the known-bug ledger.  They are not
  silently treated as complete here.
- EHCI transaction translators, split transactions, and external USB hubs are
  not added.  Directly attached low/full-speed devices use a paired UHCI
  companion; high-speed interrupt endpoints use the EHCI periodic schedule.
- USB HID Report Protocol, input events, console delivery, and physical HID
  acceptance remain owned by WS006.
- Shared-INTx subscriber removal currently relies on the owning driver to
  quiesce its device-local interrupt source first.  The generic PCI layer does
  not yet set and restore the function's `PCI_COMMAND.INTx Disable` bit.  A
  future caller which unlinks a still-asserted function could therefore hold a
  shared level active after its handler is gone.  EHCI and UHCI satisfy the
  required pre-quiesce ordering, so this is a known generic PCI follow-up, not
  a reason to weaken or misstate the p031 HCD result.

## Production audit baseline

The implementation starts from the following observed source boundaries:

- `include/drivers/usb.h` already defines concurrent ownership as one active
  URB per endpoint, defines exact enqueue/dequeue ownership, exposes endpoint
  descriptors including `bInterval`, and publishes
  `drv_usb_hcd_root_hub_changed()`.  No function driver needs an HCD name,
  private controller type, or new UAPI.
- `src/drivers/usb.c` already owns submit, cancel, terminal publication,
  callback drain, device destruction, and root-port re-enumeration.  The root
  change routine takes the USB topology lock and performs synchronous control
  requests, so an IRQ handler must never invoke it directly.
- `src/drivers/pci-uhci.c` has one controller-global `active` request and one
  boolean `submitting` gate.  Every frame-list entry points at that one request,
  retirement terminates all 1,024 entries, request construction does not
  distinguish an interrupt schedule or use `bInterval`, IRQ processing scans
  only the global request, and roots are scanned only by the platform-time
  probe.
- `src/drivers/pci-ehci.c` has the same single `active`/`submitting` boundary.
  The periodic list is left empty, all requests are attached as one async QH,
  request construction hardcodes high-speed async-QH fields, Async Advance
  resets the only link, IRQ processing scans only one request, and the
  port-change status bit is acknowledged without deferring a root rescan.
- `ws004-p016` proves retirement for one legacy request.  Its runtime evidence
  explicitly excludes root hotplug and its request-local proof must be
  preserved while the schedule becomes concurrent.
- The existing xHCI concurrency source gate deliberately rejects the legacy
  capability bit, and the p016 QEMU runner deliberately excludes hotplug.
  HW-T25 must replace those stale assertions with positive legacy evidence.
- The paired q35/OVMF topology routes its EHCI and three UHCI controllers over
  two shared legacy INTx lines.  The PC/AT PCI host returns only the line
  number, while each x86 HAL line accepts one handler.  PCI must therefore own
  one physical-line dispatcher with multiple device cookies; otherwise only
  the first EHCI/UHCI owner on each line can attach.

## Frozen public contract

The existing USB HCD interface remains the architectural boundary:

- `urb_enqueue() == 0` accepts exactly one HCD owner and eventually produces
  exactly one terminal completion, unless checked dequeue returns ownership to
  the USB core.
- UHCI and EHCI accept one active URB per endpoint.  Endpoint zero is one
  serialized endpoint per USB device.  A second request for the same endpoint
  returns `EBUSY` without changing the accepted owner or schedule.
- Requests on different endpoints or devices are independent.  Control,
  interrupt-IN, bulk-IN, and bulk-OUT traffic may coexist, complete in any
  order, and cancel independently.
- Completion routing uses exact request, URB, and endpoint identity.  A late,
  duplicate, or unrelated interrupt cannot complete or free another request.
- Successful dequeue and normal completion release request DMA only after the
  controller-specific checked retirement boundary.  An uncertain boundary
  returns an error and retains the complete request/DMA graph.
- After all HW-T25 gates pass, both HCDs advertise
  `DRV_USB_HCD_CAP_CONCURRENT_URBS`.  They do not advertise it during a partial
  conversion.
- `drv_usb_hcd_root_hub_changed()` remains a worker-context operation which may
  sleep and perform enumeration.  Root-port interrupt or polling code only
  latches work and wakes the owning worker.
- EHCI and UHCI quiesce controller-local interrupt generation before checked
  PCI IRQ removal.  Removing one shared-INTx cookie must leave peer handlers
  and the physical line active; only the final cookie may mask and remove the
  HAL line handler, after an in-flight dispatch has drained.
- No HCD name comparison, ops-table identity comparison, controller private
  data access, HID-specific admission API, or public schedule object is exposed
  to function drivers.

No public USB header change is expected.  A required public HCD operation or a
change to the meaning of the existing capability crosses the reconsideration
boundary.

## Internal request and admission contract

Each controller replaces the single active pointer with an explicit request
set.  Every request records at least:

- its exact URB and endpoint owner;
- transfer/schedule class;
- controller schedule links and current linked/unlinked state;
- completion, cancellation, retirement, failure, and publication state;
- transfer direction, toggle/accounting data, and DMA allocations; and
- a retirement-queue link independent of its execution-schedule links.

The controller lock protects schedule publication, endpoint-owner admission,
terminal claims, and retirement queues.  Request allocation and descriptor
construction occur outside that lock.  The old boolean `submitting` rejection
is replaced by a closeable controller admission gate and an in-flight builder
count:

1. An enqueue enters the gate, allocates and builds its private request, then
   publishes under the controller lock only if the controller is still open
   and its endpoint has no owner.
2. Concurrent builders for unrelated endpoints are permitted.  Internal
   serialization needed by a controller allocator may wait; it may not return
   controller-global `EBUSY` merely because another endpoint is being built.
3. A failed or losing publication frees only the unpublished request.
4. Quiesce closes the gate, joins all builders, claims or drains every accepted
   request, joins retirement and root workers, and only then enters the p009
   controller/IRQ/DMA barrier.

IRQ code scans the active request set after a USB completion/error indication,
claims every newly terminal request under the controller lock, and queues
retirement.  Retirement and completion callbacks run outside that lock.  One
request's cancellation, completion callback, or replacement submission must
not wait on itself or disturb another endpoint.

## UHCI schedule and retirement contract

UHCI uses a persistent controller-owned schedule instead of rewriting all frame
entries for each request:

- A periodic skeleton provides validated low/full-speed interrupt cadence from
  the endpoint descriptor's `bInterval`.  Multiple interrupt endpoints may be
  linked without placing a permanently NAKing endpoint ahead of all other
  traffic in an unbounded depth-first chain.
- A controller-owned asynchronous tail carries control and bulk QHs.  Periodic
  traversal reaches the asynchronous tail without making an interrupt
  completion a prerequisite for bulk progress.
- Insertion and removal update only the target request's links and the required
  predecessor/skeleton link.  Unrelated periodic and asynchronous QHs remain
  reachable and active.
- A terminal or cancelled request is first made unreachable, the unlink is
  published with the required I/O ordering, and the existing healthy raw-FRNUM
  advance is then required before QH/TD/bounce DMA is read or released.
- FRNUM all-ones/reserved states, halt, host-system error, process error, or a
  bounded failure to advance retain the affected request graph and produce no
  false terminal publication.  A controller-wide quarantine is allowed when
  the hardware state makes isolation unprovable.
- FRNUM wrap and two or more requests entering retirement in adjacent frames
  are explicit HW-T25 cases.

The p016 implementation which terminates all 1,024 frame entries is therefore
not retained as the per-request unlink operation.  Its checked hardware
observation and fail-closed policy are retained.

## EHCI schedule and retirement contract

EHCI separates asynchronous and periodic work:

- Control and bulk QHs form a controller-owned circular asynchronous ring.
  Insertion and unlink preserve the head and every unrelated QH.
- Completed/cancelled async requests enter a retirement queue.  Only one
  software generation owns the Async Advance doorbell at a time; stale IAA is
  cleared and read back before IAAD, and only the matching new acknowledgement
  retires that request.  Other async QHs continue running during the handshake.
- High-speed interrupt endpoints are placed in the periodic schedule according
  to validated `bInterval` cadence and legal service-mask fields.  Periodic
  schedule operation is enabled only after its frame list is complete.
- IAA is not evidence for periodic unlink.  The first implementation may use a
  bounded checked periodic-schedule disable/PSS-clear/update/re-enable barrier;
  alternatively it may use a specification-backed FRINDEX observation.  It may
  not substitute an arbitrary delay.  A finite checked periodic pause must not
  stop the asynchronous storage ring.
- USBINT/USBERR processing scans all linked requests and claims only requests
  whose own qTD/QH state is terminal.  Multiple terminal requests from one IRQ
  are retired exactly once.
- A direct low/full-speed root device is handed to the UHCI companion through
  the EHCI port-owner path.  The paired UHCI root worker performs enumeration.
  Absence of a companion is reported honestly; this Phase does not synthesize
  unsupported split transactions.

## Runtime root-port lifecycle

Each legacy controller has root-event state independent of request retirement:

- EHCI names and handles the Port Change Detect status bit.  The IRQ handler
  acknowledges/latches it, sets one coalescing pending flag, and wakes the root
  worker without acquiring the USB topology lock.
- UHCI has no dedicated root-port change interrupt.  Its root worker polls
  PORTSC at a bounded cadence, initially 100 ms, compares connection/change
  state, and invokes the common root change routine only when needed.  A stop
  notification wakes it immediately rather than waiting for the next poll.
- Initial platform root probing remains synchronous so boot storage is
  available at its current point in startup.  Successful initial probing marks
  runtime root dispatch ready; stale pre-ready indications are coalesced into
  one later scan.
- The root worker calls the existing USB-core destroy/re-enumerate path.  It
  does not duplicate address assignment, descriptor parsing, driver detach,
  binding, or device publication.
- Disconnect first closes device/interface admission, then class-driver
  teardown cancels and drains active requests.  Reinsert creates a new USB
  device generation and may reuse an address only after the old owner is fully
  released.
- The root worker is not the request-retirement worker.  Enumeration performs
  synchronous control requests which themselves require retirement progress;
  combining the workers would deadlock.
- Attach failure unwinds a not-started or started root worker exactly once.
  Detach closes root-event admission, wakes and joins the worker before HCD
  unregister or controller memory release.  System shutdown must likewise
  avoid joining a root worker while holding the topology lock on which that
  worker is blocked.  Stop-before-unregister or a USB-core-owned deferred queue
  is acceptable; an unproven lock-order shortcut is not.

## HW-T25 deterministic test contract

Add a reusable production-source/model fixture under the WS004 test directory,
with a runner named `run-legacy-hcd-concurrent-hotplug-test.sh`.  It covers at
least:

- control plus persistent NAKing interrupt-IN plus bulk-IN/bulk-OUT requests,
  with every relevant terminal ordering and storage progress while interrupt
  input remains pending;
- two requests on different endpoints accepted concurrently and a second
  request on the same endpoint rejected without mutation;
- simultaneous builders, publication versus quiesce, cancellation versus IRQ,
  callback resubmission, callback cancellation, duplicate/late IRQ, and one
  terminal publication per accepted request;
- UHCI periodic cadence, async-tail reachability, request-local unlink, FRNUM
  wrap, adjacent retirements, invalid/stalled FRNUM, and preservation of every
  unrelated QH;
- EHCI async-ring insertion/removal, serialized fresh IAA generations, stale,
  duplicate, or absent IAA represented as the controller's untagged W1C status
  bit after stale-bit clear/readback, high-speed `bInterval` exponent mapping,
  legal service masks and tree cadence, checked periodic unlink, and continued
  async storage progress during periodic retirement;
- port-change coalescing, disconnect with a pending interrupt request,
  reinsert on the same port as a new device generation, and no callback or
  request from the old generation reaching the new one;
- root-worker start/stop/unwind, stop while polling, stop while an event is
  pending, topology-lock contention, request-worker independence, controller
  detach, shutdown, and callback re-entry;
- allocation failure at each request and controller-schedule allocation, HCD
  registration failure, partial retirement/root/HID worker startup and
  publication failure, checked join failure, and exact reverse-order cleanup;
  and
- at least 100 deterministic connect/detach/reinsert iterations with balanced
  request, URB, DMA, worker, device, and endpoint ownership.

Run the fixture normally, under ASan/UBSan, and through GCC `-fanalyzer`.  Its
production-source audit must reject a controller-global active request, an
all-frame UHCI per-request unlink, an EHCI one-QH reset, a root scan from IRQ
context, shared root/retirement worker identity, and premature capability
advertisement.

The p016 checked-retirement fixture remains a regression but must be extended
from one active request to request-local retirement queues.  The p011 xHCI
source gate must stop requiring legacy single-flight and instead require the
capability only after HW-T25's positive source/model gates pass.

## QEMU runtime gate

The production USB HID driver does not yet exist, so HW-T25 uses a test-only
checkpoint driver linked only by the dedicated legacy-HCD test configuration.
It matches USB class `03`, validates one interrupt-IN endpoint, keeps one URB
armed, and emits bounded submit/completion/detach markers.  It does not publish
evdev, decode reports, provide console policy, or enter an ordinary build.

The QEMU runner force-rebuilds canonical `build/amd64/hdd-image.img` from the
current checkout with `make -B -j16` and the Phase-owned configuration before
hashing or copying it.  This amd64 UEFI image uses IDE root so the tested USB
storage device is an independent workload.  Run these two cells:

1. **UHCI cell:** `piix3-usb-uhci` with read-only `usb-storage` on port 2.
   After login through the emulated PS/2 keyboard, QMP hot-adds a `usb-mouse`
   on port 1 for the checkpoint generations.
2. **Paired EHCI/UHCI cell:** `ich9-usb-ehci1` plus
   `ich9-usb-uhci1`, `ich9-usb-uhci2`, and `ich9-usb-uhci3` using
   `masterbus=ehci.0` and `firstport=0`, `2`, and `4`.  Attach high-speed
   storage on shared port 6, then QMP hot-add a low/full-speed `usb-mouse` on
   shared port 1 after PS/2 login and prove the companion handoff.

The mouse in the paired cell intentionally proves USB 1.1 companion
handoff and UHCI interrupt-IN operation; it does not exercise an EHCI
high-speed interrupt endpoint.  Until a deterministic high-speed interrupt-IN
test device is added, EHCI periodic scheduling and checked periodic unlink are
accepted only from the production-source and HW-T25 model gates.  The runtime
evidence must label that limitation and must not claim EHCI periodic I/O.

For each cell the runner must:

1. Reach the login marker with the expected controllers and storage attached,
   while the USB checkpoint device is absent.  Drive the PS/2 login and shell
   only through QMP `input-send-event` commands containing explicit key-down
   and key-up pairs, then hot-add the checkpoint `usb-mouse`.
2. Use QMP `human-monitor-command` with `mouse_move 1 0` to obtain real mouse
   interrupt completions before, during, and after a throttled 4-KiB read from
   the USB-storage device.
3. Use QMP-wrapped `device_del` and `device_add` commands to perform 11 checked
   detach/reinsert transitions, producing 12 distinct checkpoint generations
   without losing storage access or allowing an old-generation callback into
   a new owner.
4. Re-read and compare the storage payload, then leave generation 12 re-armed
   with sequence 2 pending across the normal reboot boundary.  Its clean
   detach and the root/retirement worker joins must precede controller/DMA
   release.

The failure oracle includes timeout, controller halt/HSE/process error,
unmatched or duplicate terminal ownership, request-retirement failure,
retained DMA on the success path, storage checksum mismatch, stale-generation
callback, topology deadlock, panic, and failure to reboot.  Faults which are
deliberately injected to prove fail-closed retention are recorded separately
and are not mistaken for a successful teardown.

## Detailed procedure

1. Implement HW-T25's scheduler, ownership, retirement-queue, root-worker, and
   allocation-failure models before changing the HCDs.
2. Replace UHCI global admission with the request set and builder gate.  Add the
   periodic/async skeleton, interval placement, multi-request IRQ scan, local
   unlink, checked FRNUM retirement, and multi-request quiesce.
3. Replace EHCI global admission with the request set and builder gate.  Add the
   circular async ring, serialized retirement/IAA queue, high-speed periodic
   schedule and retirement barrier, multi-request IRQ scan, and multi-request
   quiesce.
4. Add independent EHCI event-driven and UHCI polling root workers with checked
   attach failure, detach, shutdown, and topology-lock ordering.
5. Set the existing concurrent-URB capability on each HCD only after its
   complete request and worker conversion passes HW-T25.
6. Add the test-only interrupt probe and the two non-interactive QEMU cells,
   including storage coexistence, 11 detach/reinsert transitions, and 12
   checkpoint generations.
7. Run the p016 retirement, p015 binding, p011 xHCI concurrency, USB function,
   URB publication, USB-storage SCSI, and system-shutdown regressions.
8. Compile configured amd64 UEFI and i386 PC/AT production HCD/core/storage
   objects, run the repository build, inspect scoped links/source references,
   and record exact image/tool hashes and QEMU commands in phase evidence.

## Focused verification commands

Queue `q047` uses a Queue-local temporary directory. The intended focused
commands are:

```sh
mkdir -p build/q047-tmp

TMPDIR="$PWD/build/q047-tmp" \
  plan/ws004-hardware/tests/run-legacy-hcd-concurrent-hotplug-test.sh

TMPDIR="$PWD/build/q047-tmp" \
  plan/ws004-hardware/tests/run-legacy-hcd-retirement-test.sh

TMPDIR="$PWD/build/q047-tmp" \
  plan/ws004-hardware/tests/run-xhci-concurrent-urbs-test.sh

TMPDIR="$PWD/build/q047-tmp" \
  plan/ws004-hardware/tests/run-usb-binding-transactions-test.sh

TMPDIR="$PWD/build/q047-tmp" \
  cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  src/drivers/pci.c \
  plan/ws004-hardware/tests/pci-shared-intx-test.c \
  -o build/q047-tmp/pci-shared-intx-test
build/q047-tmp/pci-shared-intx-test

TMPDIR="$PWD/build/q047-tmp" \
  plan/ws004-hardware/tests/run-legacy-hcd-concurrent-hotplug-qemu.sh \
  build/data.img build/q047-legacy-hcd

TMPDIR="$PWD/build/q047-tmp" make -j16

git diff --check -- \
  include/drivers/pci.h include/drivers/usb.h \
  src/drivers/pci.c src/drivers/usb.c \
  src/drivers/pci-uhci.c src/drivers/pci-ehci.c \
  plan/ws004-hardware
```

The implementation Queue must also run the existing focused USB function,
URB-publication, USB-storage SCSI, and shutdown runners named by the WS004 test
index.  It must not run `make check`, consume `.internal/`, or modify Noct
sources.

## Completion conditions

- HW-T25 passes normally, with ASan/UBSan, and with GCC analyzer coverage.  Its
  100 synthetic lifecycle iterations have balanced ownership and no stale
  request, callback, device generation, worker, or DMA reference.
- UHCI and EHCI each accept independent endpoint owners; a NAKing interrupt-IN
  request cannot block control or bulk storage progress, and same-endpoint
  queue depth remains the explicit `EBUSY` boundary.
- UHCI local unlink plus healthy FRNUM observation and EHCI async/periodic
  checked retirement preserve every unrelated request.  No arbitrary delay is
  accepted as DMA-retirement proof.
- EHCI port-change events and UHCI bounded polling detach and re-enumerate a
  root device only in worker context.  Root and retirement workers remain
  independent and have a checked, deadlock-free stop/join lifetime.
- Both HCDs advertise `DRV_USB_HCD_CAP_CONCURRENT_URBS`, and function drivers
  remain independent of HCD implementation identity.
- The PCI shared-INTx fixture proves one HAL registration and one EOI per line,
  delivery to every subscriber, non-final removal without masking peers,
  in-flight `EBUSY` retention, and final checked-drain failure followed by a
  successful retry.  It does not claim generic removal of a still-asserted
  function; p031 relies on the EHCI/UHCI pre-quiesce contract recorded above.
- The standalone UHCI and paired EHCI/UHCI QEMU cells reach login, complete HID
  interrupt probes and storage reads concurrently, pass 11 automatic
  detach/reinsert transitions across 12 generations, verify the storage
  payload, and reboot without panic, stale completion, worker leak, or
  DMA-retention diagnostics.
- The paired cell records that its low/full-speed HID is owned by UHCI; EHCI
  high-speed periodic scheduling remains production-source/model evidence in
  this Phase and is not misreported as QEMU runtime coverage.
- Configured amd64 UEFI and i386 PC/AT production builds, repository
  `make -j16`, and the existing xHCI, binding, retirement, storage, publication,
  and shutdown regressions pass.
- Exact QEMU version, topology, commands, forced canonical-image build command,
  image/config hashes, marker counts, failure oracle, and deliberately
  uninjectable hardware conditions are kept in a phase-owned evidence record.

Meeting these conditions clears only the legacy concurrency/hotplug
prerequisite of `ws006-p008`.  USB HID implementation remains blocked until the
separate reset/STALL Phase also completes.

## Current result

Complete (`q047`).  HW-T25 passes in its ordinary, ASan/UBSan, and GCC analyzer
modes, including 100 balanced synthetic lifecycle iterations and the positive
production-source gates.  Its configured amd64 UEFI and i386 PC/AT production
gates pass.  The legacy-HCD retirement, xHCI concurrent-URB, USB binding,
system-shutdown, HW-T27 console, USB-storage SCSI, URB-publication, and PCI
shared-INTx fixtures also pass.  The repository `make -j16` production build
passes.

The forced-canonical QEMU run retained at `build/q047-legacy-hcd-final4` is a
runtime **PASS** for both the standalone UHCI cell and the paired EHCI plus
three-UHCI cell.  QEMU 10.0.11 booted the fresh Phase-configured image, and the
paired guest attached all four legacy controllers through the PCI shared-INTx
dispatcher.  Each cell recorded 12 checkpoint attaches and clean detaches, 11
detach/reinsert transitions, 12 or more successful mouse interrupt
completions, four accepted and four completed Storage requests, payload
comparison, a final re-arm pending across reboot, and checked worker joins.
The canonical boot and auxiliary-image hashes were unchanged and the runner
returned `acceptance_exit_status=0`.

The exact final runner invocation was:

```sh
TMPDIR="$PWD/build/q047-tmp" \
  plan/ws004-hardware/tests/run-legacy-hcd-concurrent-hotplug-qemu.sh \
  build/data.img build/q047-legacy-hcd-final4
```

It force-built the source image with `make -B -j16` and
`ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-amd64-legacy-hcd.mk`. The
configuration SHA-256 was
`34293c880bb516fcfa6e4e45690bf24bd212fd019796f329ab264c07160a8690`, the
canonical boot-image SHA-256 before and after the run was
`9ae50624558cec3d7b6d83d6bd2ff1862c47042dcddbbbb5f8368822ef1e741a`, and the
auxiliary-image SHA-256 before and after was
`90238b68d79ff20d38232f416c4dd1570a0c3f72e366d294a04d6ae36377b41f`.

The authoritative summaries are `metadata.txt`, `results.tsv`, and each
cell's `metadata.txt`, `controller-result.txt`, logs, and
`hid-action-boundaries.tsv` beneath that directory.  Together these results
satisfy every completion condition, clear p031 as the legacy-HCD prerequisite
of p032 and `ws006-p008`, and also supply p033's shared console-stress evidence.
The paired cell truthfully records companion-UHCI HID and high-speed EHCI
Storage; it does not claim EHCI high-speed periodic runtime coverage. Frozen
FRNUM/IAA hardware faults likewise remain deterministic model evidence.
The generic PCI shared-INTx source-disable residual remains in the known-bug
ledger: EHCI/UHCI are safe because they quiesce controller-local interrupt
generation before checked removal, while a future generic subscriber which
cannot do so still needs a separately selected solution.

## Reconsideration boundary

Stop and mark this Phase `uncleared` rather than weakening ownership if:

- the existing HCD enqueue/dequeue, endpoint descriptor, capability, and root
  change interface cannot express safe request ownership without a new public
  HCD operation;
- UHCI or EHCI periodic unlink cannot be proven through documented controller
  state and would otherwise require an arbitrary delay;
- direct low/full-speed EHCI operation requires a transaction translator,
  split transactions, or hub support.  Preserve companion-UHCI handoff and
  extract that support into another Phase;
- root-worker shutdown cannot avoid topology-lock inversion without a USB-core
  lifecycle change.  Record the required contract instead of joining a blocked
  worker or leaking it;
- QEMU cannot provide truthful interrupt, storage-coexistence, detach, or
  reinsert evidence.  Retain deterministic model evidence and record the
  missing runtime predicate separately;
- reset/STALL recovery or `BUG-003` short-transfer/error semantics becomes the
  actual blocker.  Return that result to its owning Phase rather than silently
  expanding p031; or
- safe isolation of one failed retirement is impossible.  Quarantine and
  retain the complete affected controller graph rather than freeing DMA or
  unlinking unrelated work.

No physical human checkpoint is required by p031.  WS006 owns final USB HID
device behavior and physical keyboard/pointer acceptance after both WS004
prerequisites are complete.

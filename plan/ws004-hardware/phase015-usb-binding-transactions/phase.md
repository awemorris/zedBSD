# WS004 Phase 015: general USB binding and interface transactions

Last updated: 2026-08-29

Phase ID: `ws004-p015`

Status: completed (`q027`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Replace the device-wide allocated-URB barrier with a general USB ownership
model that is valid for Mass Storage, CDC NCM, HID, Audio-style alternate
streaming, and composite devices.  An interface transition must exclude only
accepted work on that interface, while a driver binding owns its primary and
claimed interfaces throughout attach, detach, rollback, and quarantine.

## Dependencies

- `ws004-p010`: retained configurations, alternates, endpoints, claims, and
  checked endpoint rollback.
- `ws004-p011`: callback-aware URB/HCD ownership and checked drain.
- `ws004-p012`: removable child-device publication and terminal shutdown
  barriers.

`ws004-p014` remains paused at its documented reconsideration boundary until
this Phase supplies the general transaction contract.

## Frozen contract

### Ownership scopes

- A device owns configuration, reset, disconnect, address, and endpoint-zero
  control serialization.
- A binding owns one primary interface, every sibling claimed by it, its
  driver data, and the lifecycle `PROBING`, `BOUND`, `UNBINDING`,
  `DETACH_PENDING`, or `DEAD`.
- A logical interface owns its alternate-selection admission gate and accepted
  non-control URB count.  An alternate transition never waits for an idle URB
  object or for activity on an unrelated interface.
- An endpoint belongs to one retained host-interface/alternate object.  Object
  identity, not endpoint address alone, determines whether an URB is currently
  submit-eligible.

### URB and alternate behavior

- Allocating an URB pins software/device lifetime but does not activate an
  endpoint or reserve HCD execution.  An idle, terminal, fully drained URB may
  remain allocated while its interface changes alternate setting.
- A non-control submit atomically enters the endpoint interface's I/O gate,
  verifies the active configuration and exact active alternate, and retains
  that gate through HCD ownership and callback return.  Switching, inactive,
  cross-device, disconnecting, and quarantined cases fail without enqueue.
- `drv_usb_interface_set_alternate()` atomically closes only the target
  interface, rejects an in-flight target with `EBUSY` before hardware changes,
  performs checked disable/request/enable, publishes only a completed change,
  and then reopens admission.  Sibling-interface transfers continue.
- Device configuration changes remain device-wide and require no bound owner
  or accepted transfer.  They are not weakened into an interface operation.
- Endpoint zero is serialized per device.  Synchronous helpers may not release
  their URB or caller buffer until HCD ownership and callback activity are
  gone, including timeout/cancel failure paths.

### Binding lifecycle

- Probe publishes a provisional driver binding before invoking `attach`, so a
  claim and partial driver data always have a lifecycle owner.
- A nonzero attach result invokes the same driver detach path with
  `DRV_USB_DETACH_ATTACH_FAILED`.  Cleanup success clears driver data, claims,
  and the provisional binding.  Cleanup failure retains all remaining state as
  `DETACH_PENDING`, prevents reprobe/submission, and permits a later forced or
  ordinary detach retry.
- A normal detach closes binding admission before calling the driver.  A
  nonzero result retains driver, driver data, and claims in
  `DETACH_PENDING`; zero alone clears them.
- Physical disconnect closes device admission first and uses forced detach
  without requiring a control transfer back to alternate zero.  Checked HCD
  device quiesce remains the final DMA barrier.
- Ambiguous `SET_INTERFACE` or failed endpoint compensation is represented as
  quarantined/unknown rather than falsely advertising the old alternate as
  usable.  Conservative device-wide quarantine is acceptable in this Phase;
  interface-scoped recovery may be extracted later.

### Interface boundary

- Existing HCD operations remain unchanged.  Interface, binding, and function
  policy stay in USB core rather than leaking into xHCI/EHCI/UHCI.
- Do not introduce an NCM-specific alternate API, automatic driver cleanup
  stack, public transaction handle, URB rebind API, or speculative common
  class-driver backend.
- The cohesive public USB header may change only for the detach reason/state
  contract required here.  Ordinary locking, counters, and transaction
  machinery remain private to `usb.c`.

## Planned work

1. Add production-source fixtures for inactive/idle URBs, target-versus-sibling
   in-flight work, submit-versus-switch races, callback return, alternate
   rollback/quarantine, cross-device endpoints, and Audio-shaped alt0/altN
   reuse.
2. Add the interface I/O gate, endpoint-to-alternate ownership, submit
   validation, and race-free alternate publication.
3. Make device selection and endpoint-zero synchronous control serialization
   atomic and guarantee final drain before temporary URB release.
4. Add provisional binding and attach-abort/detach-pending state, with claims
   cleared only after successful cleanup.
5. Adapt Mass Storage to the tightened control/drain and provisional cleanup
   contract without changing its SCSI/BOT behavior.
6. Pass prerequisite USB, xHCI, storage, hotplug, configured-build, and ordinary
   amd64 QEMU xHCI USB-root regressions.

## Completion conditions

- An allocated inactive-alternate URB survives alt0 to altN to alt0 and is
  rejected before activation, accepted only while active, and reusable after a
  complete drain.
- A target-interface accepted URB or callback blocks its alternate transition;
  an independent Storage/HID-shaped sibling URB does not.
- Submit racing alternate selection has one atomic winner and never reaches an
  inactive HCD endpoint.
- Clean attach failure removes provisional data and claims; failed attach
  cleanup and failed normal detach retain one non-submit-capable binding which
  a forced retry can release exactly once.
- Synchronous endpoint-zero timeout/cancel paths retain all caller memory until
  ownership ends, and concurrent control users serialize without corrupting
  request state.
- Existing USB function, URB publication, xHCI, USB Storage, shutdown/hotplug,
  amd64, and configured i386 PC/AT gates pass without `make check` or
  `.internal/`.

## Reconsideration boundary

Stop and mark this Phase `uncleared` if the existing checked HCD endpoint and
device-quiesce operations cannot implement the interface gate without a new HCD
transaction API, or if provisional binding requires a public cleanup framework
whose ownership cannot be kept inside USB core.  Do not weaken callback/DMA
retention or silently restore a binding after an ambiguous hardware result.

## Result

Completed on 2026-08-29.

- USB core now anchors every endpoint to its immutable retained alternate and
  admits non-control submissions only through the exact active configuration,
  alternate, interface, binding, and device gates.  Idle URBs may remain
  allocated across alternate changes without making an inactive endpoint
  submit-eligible.
- Probe publishes a provisional binding before attach, and failed attach or
  detach retains one closed `DETACH_PENDING` binding until cleanup succeeds.
  Physical disconnect uses the private forced-detach path after joining
  binding, selection, and submit transactions.
- Endpoint zero is serialized per device.  Submit, synchronous completion,
  cancel, callback return, and HCD ownership use one terminal handoff, so a
  callback cannot free an URB while the submitting CPU still references it.
- Mass Storage retains its preallocated reclaim-safe URBs.  Its serialized
  control path retries transient reuse contention within the command timeout
  and waits for full HCD/callback drain before reuse.
- The existing HCD enqueue/dequeue ownership contract is explicit in the USB
  header.  UHCI and EHCI now serialize their single active request across SMP
  submit, IRQ, quiesce, and stop paths.  Their unproven software-only dequeue
  success was removed; cancellation conservatively returns `EBUSY` and keeps
  ownership for normal completion.

Verification evidence:

- `run-usb-binding-transactions-test.sh`: 971 checks in both ordinary and
  ASan/UBSan builds, plus GCC analyzer/source gate; a ten-run ordinary repeat
  remained stable.
- `run-xhci-concurrent-urbs-test.sh`: xHCI ordinary/sanitized fixtures, USB
  function model 1332 checks in both modes, legacy-HCD source audit, and amd64
  plus configured i386 PC/AT production objects passed.
- USB Storage SCSI, URB publication, and system-shutdown focused fixtures
  passed.
- `make -j16` passed.  A fresh disposable copy of
  `build/amd64/hdd-image.img` booted once through q35 `qemu-xhci` USB Mass
  Storage to `login:` with no USB/storage/kernel diagnostic match.
- `git diff --check` passed, and the final independent audit found no open
  P0/P1 issue within this Phase.

The audit also confirmed that legacy UHCI/EHCI request DMA needs a
controller-proven retirement boundary before normal-completion release as well
as successful cancellation.  That pre-existing controller-specific work is
recorded without weakening this Phase as pending, non-Queue
[`ws004-p016`](../phase016-legacy-hcd-request-retirement/phase.md).

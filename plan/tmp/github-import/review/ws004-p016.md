# WS004 Phase 016: legacy HCD checked request retirement

Last updated: 2026-08-31

Phase ID: `ws004-p016`

Status: complete (`q041`); residual from `ws004-p015`

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Give UHCI and EHCI a controller-proven request-retirement boundary before any
QH, TD/qTD, or bounce DMA is freed.  A software schedule unlink is not proof
that a legacy host controller has stopped using a prefetched descriptor.

`ws004-p015` makes the current implementation safe by serializing active
ownership across CPUs and making `urb_dequeue` return `EBUSY`; ownership stays
with the HCD until normal completion.  This Phase adds checked hardware
retirement rather than weakening that conservative behavior.

## Dependencies

- `ws004-p009`: checked controller quiesce and IRQ teardown.
- `ws004-p011`: exact HCD completion and callback-aware URB ownership.
- `ws004-p015`: public enqueue/dequeue ownership contract and SMP-safe legacy
  HCD active-request serialization.

## Frozen safety contract

- `urb_enqueue() == 0` transfers one request to the HCD and produces exactly
  one later completion unless a checked successful dequeue transfers terminal
  publication back to the USB core.
- `urb_dequeue() == 0` is permitted only after the controller can no longer
  fetch or write any request-owned DMA.  On uncertainty it returns nonzero and
  retains the request, URB, and every DMA allocation.
- Normal completion has the same DMA-retirement requirement as cancellation.
  Seeing a completed descriptor or interrupt is not by itself permission to
  unlink and immediately free a still-cached schedule object.
- Completion, cancellation, quiesce, and IRQ handling select one terminal
  owner under the HCD active-request lock.  Callbacks run after that lock is
  released and after request DMA ownership is resolved.
- A timeout or failed retirement check is bounded and diagnostic.  It never
  reports false success, publishes a second completion, or frees quarantined
  state.

## Planned work

1. Add a UHCI retirement state machine shared by cancellation and normal
   completion.  Unlink the QH from every frame-list entry, publish the update,
   and wait for a hardware-observed safe frame boundary before releasing its
   QH/TD/bounce DMA.
2. Add an EHCI asynchronous-schedule retirement state machine shared by
   cancellation and normal completion.  Unlink the QH, issue the Interrupt on
   Async Advance doorbell, and require the matching IAA acknowledgement before
   releasing QH/qTD/bounce DMA.
3. Define bounded failure and controller-quarantine behavior for a stalled
   frame counter, missing IAA acknowledgement, controller halt, and IRQ race.
4. Add focused ownership fixtures covering completion-versus-dequeue,
   timeout-versus-IRQ, duplicate/late interrupts, quiesce during retirement,
   and checked-failure retention.
5. Exercise production UHCI and EHCI paths in QEMU with control and bulk I/O,
   cancellation/timeouts where fault injection permits, detach/shutdown, and
   repeated request reuse.

## Completion conditions

- UHCI never frees request DMA until the frame-list unlink and a documented
  controller-observed retirement boundary have both completed.
- EHCI never frees async request DMA until unlink and the matching checked IAA
  handshake have completed.
- Both normal completion and successful cancellation use the checked
  retirement path; failure retains HCD ownership and produces no core terminal
  publication.
- SMP race fixtures prove one terminal owner and exactly one completion across
  dequeue, IRQ, quiesce, and stop interleavings.
- Configured amd64 and i386 PC/AT builds pass, and QEMU UHCI and EHCI runs
  complete enumeration, representative I/O, and checked teardown without DMA
  lifetime diagnostics.

## Reconsideration boundary

Stop and mark this Phase `uncleared` rather than approximating retirement with
an arbitrary delay.  If QEMU cannot expose a required cancellation or stale
event, retain the focused model result and record the missing hardware/runtime
evidence separately.

## Result

Completed in `q041` on 2026-08-31.

- UHCI now gives completion and cancellation one terminal owner, removes the
  QH from all 1,024 frame entries, publishes the unlink, and accepts only a
  healthy raw FRNUM transition observed while the controller is still running.
  All-ones, reserved-bit, halted, host-system-error, and process-error register
  states fail closed and retain the request graph and DMA.
- EHCI deactivates the source qTDs, waits for the controller-owned QH overlay
  to become inactive, removes the QH, clears and reads back stale IAA, and then
  requires the fresh post-IAAD acknowledgement for the current software
  generation. Missing acknowledgement, halt, HSE, or invalid MMIO quarantines
  the controller without freeing request DMA.
- Both drivers use a dedicated bounded retirement worker. A completion
  callback which submits and synchronously cancels a replacement request runs
  the same progress routine inline instead of waiting on its own worker.
  Successful data progress is committed to the endpoint toggle after the
  checked boundary for completion, partial/error retirement, and cancellation.
- The QEMU gate exposed and corrected an independent USB-core regression:
  legacy wire `SET_ADDRESS` must be constructed for default address zero while
  the newly allocated address remains reserved for checked failure cleanup.
  The explicit xHCI `device_set_address` path is unchanged.
- The focused model passes 8,189 checks normally and under ASan/UBSan, plus GCC
  analyzer and production-source gates. Configured amd64 UEFI and i386 PC/AT
  driver builds, the xHCI concurrency regression, USB binding regression,
  shutdown-order gate, repository `make -j16`, and `git diff --check` pass.
- QEMU 10.0.11 boots the same checked image once with UHCI and once with EHCI;
  each enumerates read-only USB storage, transfers 4 KiB through the production
  bulk path, reports its checked-retirement marker, and completes reboot
  teardown without a DMA-retention diagnostic. Exact commands and hashes are
  in [q041 legacy-HCD evidence](../tests/q041-legacy-hcd-evidence.md).

QEMU does not inject a frozen UHCI FRNUM or stale/duplicate/missing EHCI IAA,
and the legacy controllers do not yet dispatch runtime root-port hotplug.
Those paths remain deterministic model evidence and are stated as absent in
the runtime metadata; hot-unplug was not used as false cancellation evidence.
The pre-existing UHCI short-IN/control-status semantics and EHCI halted-error
classification are recorded in the known-bug ledger and are not claimed here.

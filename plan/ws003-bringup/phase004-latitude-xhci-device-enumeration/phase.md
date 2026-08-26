# WS003 Phase 004: Latitude xHCI device enumeration

Last updated: 2026-08-26

WSID: `ws003`

Phase ID: `p004`

Combined ID: `ws003-p004`

Status: Ready; not present in an approved Queue

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md)

Evidence: [Latitude xHCI evidence](../tests/latitude-xhci-evidence.md)

## Objective

Move the Latitude 5320 from two attached xHCI 1.2 host controllers to U2 by
making control-transfer TDs, root-port reset completion, EP0 context updates,
and timeout cancellation conform to the xHCI state machine, then enumerate the
boot USB mass-storage device as `sda`.

## Baseline and confirmed boundary

The single q012 physical run proves `ws003-p003` corrected the former
capability failure. Both `0000:00:0d.0` and `0000:00:14.0` report
`version=0120`, `reject=00000000:ok`, and a registered xHCI HCD. The relocated
64-KiB BAR for `00:14.0` is reachable.

Device enumeration fails before any storage-class probe:

```text
xhci: transfer completion=6 residual=8 length=8 slot=1 endpoint=1 direction=in
usb1: port 6 enumeration failed (44)
xhci: transfer completion=4 residual=8 length=8 slot=2 endpoint=1 direction=in
usb1: port 8 enumeration failed (5)
usb1: port 10 enumeration failed (42)
xhci: command 15 failed, completion=19
xhci: endpoint 1 stop failed during cancel (5); retaining DMA buffer
usb1: port 13 enumeration failed (5)
boot: waiting up to 5 seconds for boot storage
```

Completion 6 is Stall, completion 4 is USB Transaction Error, and command 15
completion 19 is a Stop Endpoint Context State Error. The latter occurs during
timeout cancellation and is a secondary recovery failure, not the first
transfer failure. The logged `endpoint=1` is DCI 1, the Default Control
Endpoint, not USB endpoint address 1.

## Confirmed implementation defects and hypotheses

| Priority | Item | Evidence and required discriminator |
| --- | --- | --- |
| D1 | Control-transfer TRBs violate reserved-zero and TD-boundary rules | The Setup Stage writes a TD Size value into reserved bits and sets CH. The one-TRB Data Stage sets TD Size 1 and chains into the Status Stage even though Setup, Data, and Status are distinct TDs. QEMU xHCI 1.0 accepts this; the physical xHCI 1.2 controller need not. A shared builder fixture must assert every DWORD and reserved bit. |
| D2 | Root-port reset uses a fixed delay without observing completion | USB core waits 50 ms but does not require `PR=0`, the reset-change indication, and `PED=1` while connection remains present. It then issues an ineffective PORT_ENABLE request. Record the final PORTSC state and replace delay-only completion with a bounded state poll. |
| D3 | EP0 parameters are not updated from the first descriptor | Full-Speed `bMaxPacketSize0` is stored only in the generic USB object and not rebuilt into the xHC EP0 input context before the non-BSR Address Device command. SuperSpeed encodes 512 as exponent 9. Control EP Average TRB Length is also incorrectly set to Max Packet Size instead of 8. |
| D4 | Timeout cancellation loses recoverable request ownership | The active/HCD pointers are cleared before Stop Endpoint. If the endpoint is already Halted or Error and Stop returns Context State Error, the DMA buffer is intentionally retained but no durable owner remains. USB teardown can then ignore Disable Slot failure and free the ring/context while DMA may still be live. Track endpoint state, retain the request, require successful recovery/Disable Slot, or quarantine the controller before reclamation. |
| D5 | Command Completion is not associated with its command TRB | The polling path accepts the first type-33 event without comparing its command-TRB pointer, while the IRQ path consumes and discards type-33 events. A focused interleaving fixture must prove command completion ownership or the event path must route completions to the waiter. |

## Scope

- Refactor control Setup/Data/Status TRB construction into shared, directly
  testable arithmetic and make all reserved fields zero.
- Make root-port reset completion state-based and bounded; retain a precise
  port/PORTSC failure record.
- Update the EP0 context through the command valid for the current Slot state
  before the next control stage, including the SuperSpeed exponent rule.
- Set Control endpoint Average TRB Length to 8.
- Track transfer endpoint state sufficiently to recover Halted/Error/Stopped
  endpoints and retain request/DMA ownership until cancellation is safe.
- Prevent device/ring/context teardown until Disable Slot succeeds or the
  entire controller has reached a proven quiescent quarantine boundary.
- Associate every Command Completion Event with the submitted command TRB so
  an IRQ/poll interleaving cannot consume the wrong completion.
- Add request-stage diagnostics which identify controller, port, slot, USB
  request, completion code, endpoint state, and recovery operation without
  unbounded logging.
- Preserve the QEMU BIOS and 4/8/16-GiB OVMF USB-root baseline.
- Perform one intermediate Latitude boot only after all agent-executable gates
  pass; final five-run repeatability remains BR-T30.

## Non-goals

- PCI BAR/capability redesign already closed by `ws003-p003`.
- USB-storage SCSI/BOT changes before a device reaches configured state and a
  storage-class probe demonstrates a separate failure.
- General USB hub topology, isochronous transfers, USB HID, USB4/Thunderbolt,
  hotplug, suspend/resume, or performance tuning.
- Masking controller errors with unconditional retries or longer fixed sleeps.
- Expanding the one-request-per-controller implementation unless the physical
  failure or a focused fixture proves that concurrency is required for U2.

## Dependencies

- Partial `ws003-p003`, whose PCI/xHCI attach changes and diagnostics remain
  the accepted baseline.
- Existing WS004 xHCI model, PCI, URB, heap, and USB-root regression fixtures.
- One disposable USB boot device and one user-operated Latitude run after the
  software candidate is frozen.

## Fixed decisions

- A no-data control request emits Setup and Status as two distinct TDs. A data
  request emits Setup, one Data TRB, and Status as three distinct TDs.
- Setup Stage uses transfer length 8, IDT, and TRT only; its reserved TD Size
  and CH fields remain zero.
- The current single Data Stage TRB uses TD Size 0 and does not chain to the
  Status Stage. Status carries IOC and the direction opposite the data stage.
- Control endpoint Average TRB Length is 8.
- Port reset success is not inferred from elapsed delay alone. It requires a
  still-connected port, reset deasserted, and an enabled port within a bounded
  deadline; reset-change acknowledgement is explicit and PORT_ENABLE is not
  used as an enable operation on xHCI.
- The first eight device-descriptor bytes are validated before updating the
  xHC EP0 context with the command valid for the current Slot state; updating
  only the generic USB object is insufficient. Full/Low/High-Speed sizes are
  byte values; SuperSpeed exponent 9 becomes 512.
- Transfer completion 4 or 6 updates the tracked endpoint state. Cancellation
  chooses Reset Endpoint and Set TR Dequeue according to that state; it never
  drops the last request/DMA owner merely because Stop Endpoint failed.
- Set TR Dequeue or Disable Slot failure does not release request, ring,
  context, or DMA memory. A controller which cannot be recovered is quiesced
  and retained using the p003 quarantine contract.
- QEMU success alone does not close this Phase. One physical `usb-storage: sda`
  observation is the intermediate U2 gate.

## Expected files and subsystems

- `drivers/pci-xhci.c`
- a small shared xHCI transfer/context helper under `include/drivers/`
- `drivers/usb.c` and `include/drivers/usb.h` for reset completion and EP0
  packet-size handoff if the HCD interface needs it
- `plan/ws003-bringup/tests/` fixtures, runbook, and evidence
- existing WS004 xHCI/USB fixtures where a shared regression must be extended

## Ordered work packages

- [ ] Add BR-T27 exact-DWORD fixtures for no-data and IN/OUT data control
      transfers, including every reserved-zero, TRT, DIR, IOC, CH, and TD Size
      field.
- [ ] Correct Setup/Data/Status construction and preserve event-pointer and
      residual matching for the resulting independent TDs.
- [ ] Add a bounded port-reset state helper and BR-T27 cases for success,
      disconnect, never-cleared reset, non-enabled completion, and timeout.
- [ ] Decode and validate `bMaxPacketSize0`, update EP0 through the state-valid
      xHC command before the next control stage, and assert Control Average TRB
      Length 8.
- [ ] Add BR-T28 endpoint-state/cancellation fixtures for Running, Halted,
      Error, and Stopped states, including Stop completion 19. Keep request and
      DMA ownership reachable until recovery or controller quarantine; inject
      Set TR Dequeue and Disable Slot failures and prove no live resource is
      freed.
- [ ] Route or match Command Completion Events by their command-TRB pointer and
      cover an IRQ/poll interleaving in BR-T28.
- [ ] Add bounded enumeration-stage diagnostics and completion-code names.
- [ ] Run BR-T27/BR-T28, applicable WS004 regressions, `make -j16`, and
      `git diff --check`. Do not use `make check` or `.internal/` material.
- [ ] Run the legacy-BIOS q35/xHCI USB-root control and BR-T24 OVMF USB-root at
      4, 8, and 16 GiB with one frozen production image.
- [ ] Run BR-T34 once on the Latitude. Record controller/BDF, port, slot, the
      first descriptor and address/configuration stages, `usb-storage: sda`,
      and the first later stop if U2 succeeds but U3 does not.
- [ ] Record the highest physical U-tier and update P/W/M/Q evidence. Defer the
      final five consecutive boots to BR-T30.

## Acceptance cases

- `BR-T27`: host control-transfer/context/reset fixture proves the exact xHCI
  1.2 TRB words, reserved-zero contract, EP0 packet-size conversion, Average
  TRB Length, and reset-state outcomes.
- `BR-T28`: host cancellation fixture proves every endpoint-state path retains
  request/DMA ownership until recovery succeeds and does not loop on Context
  State Error; command events are matched and Disable Slot failure cannot lead
  to live-DMA release.
- Existing applicable WS004 xHCI, USB URB, PCI, and heap fixtures pass.
- `BR-T21` and `BR-T24`: the frozen image reaches `login:` by USB root under
  legacy BIOS and OVMF at 4, 8, and 16 GiB.
- `BR-T34`: one Latitude cold boot enumerates the intended boot USB device and
  prints `usb-storage: sda` without an EP0 transfer failure or boot-storage
  timeout. A later root/VFS failure is recorded but does not erase U2.

## Completion conditions

- Control-transfer TRBs and EP0 contexts satisfy the declared xHCI 1.2
  reserved-bit, TD-boundary, packet-size, and Average TRB Length contracts.
- Port reset completion is state-based, bounded, and diagnosed.
- Timeout and Halted/Error endpoint recovery have durable ownership and focused
  regression evidence.
- All declared host, QEMU, build, and formatting gates pass.
- One Latitude run reaches `usb-storage: sda`, proving U2. Repetition remains
  the final BR-T30 campaign rather than an implementation blocker.

## Actual results and evidence

Planning only. This Phase was extracted from the q012 BR-T33 result and is not
authorized for implementation by q012.

## Interruption / resumption

Create a new finite Queue containing `ws003-p004` before changing code. Begin
with BR-T27 and the Control TRB correction; do not begin with the secondary
Stop Endpoint completion 19 or storage-class code.

## Remaining debt and handoff

- If the spec-correct control path still fails on only one physical port or
  speed, parse Supported Protocol capabilities and extract protocol-specific
  root-port handling rather than accepting arbitrary Speed IDs.
- If `usb-storage: sda` appears and the next stop is root selection or I/O,
  hand it to the existing U3/U4 BR-06/BR-T31 boundary.
- Multi-device concurrency, hub topology, and broader endpoint scheduling
  remain later common USB/xHCI work unless required for the boot device.

## Reference

- [Intel xHCI Requirements Specification 1.2b](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf), especially sections 4.3.1, 4.6.9, 4.8.3, 4.11.2.2, 4.11.2.4, 6.2.3, and 6.4.1.2.

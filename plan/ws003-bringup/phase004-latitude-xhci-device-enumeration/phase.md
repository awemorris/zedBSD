# WS003 Phase 004: Latitude xHCI device enumeration

Last updated: 2026-08-26

WSID: `ws003`

Phase ID: `p004`

Combined ID: `ws003-p004`

Status: Complete (`q013`)

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md)

Evidence: [Latitude xHCI evidence](../tests/latitude-xhci-evidence.md)

## Objective

Move the Latitude 5320 from two attached xHCI 1.2 host controllers to U2 by
making control-transfer TDs, root-port reset completion, EP0 context updates,
and Control endpoint parameters conform to the xHCI state machine, then
enumerate the boot USB mass-storage device as `sda`.

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
Endpoint, not USB endpoint address 1. Command/cancel/DMA safety exposed by the
same photograph is owned and completed first by `ws003-p005` in q013.

## Confirmed implementation defects and hypotheses

| Priority | Item | Evidence and required discriminator |
| --- | --- | --- |
| D1 | Control-transfer TRBs violate reserved-zero and TD-boundary rules | The Setup Stage writes a TD Size value into reserved bits and sets CH. The one-TRB Data Stage sets TD Size 1 and chains into the Status Stage even though Setup, Data, and Status are distinct TDs. QEMU xHCI 1.0 accepts this; the physical xHCI 1.2 controller need not. A shared builder fixture must assert every DWORD and reserved bit. |
| D2 | Root-port reset uses a fixed delay without observing completion | USB core waits 50 ms but does not require `PR=0`, the reset-change indication, and `PED=1` while connection remains present. It then issues an ineffective PORT_ENABLE request. Record the final PORTSC state and replace delay-only completion with a bounded state poll. |
| D3 | EP0 parameters are not updated from the first descriptor | Full-Speed `bMaxPacketSize0` is stored only in the generic USB object and not rebuilt into the xHC EP0 input context before the non-BSR Address Device command. SuperSpeed encodes 512 as exponent 9. Control EP Average TRB Length is also incorrectly set to Max Packet Size instead of 8. |

## Scope

- Refactor control Setup/Data/Status TRB construction into shared, directly
  testable arithmetic and make all reserved fields zero.
- Make root-port reset completion state-based and bounded; retain a precise
  port/PORTSC failure record.
- Update the EP0 context through the command valid for the current Slot state
  before the next control stage, including the SuperSpeed exponent rule.
- Set Control endpoint Average TRB Length to 8.
- Add request-stage diagnostics which identify port, slot, USB
  request, completion code, endpoint state, and recovery operation without
  unbounded logging.
- Preserve the QEMU BIOS and 4/8/16-GiB OVMF USB-root baseline.
- Perform exactly one shared Latitude boot only after all agent-executable
  gates pass; final five-run repeatability remains BR-T30.

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
- `ws003-p005`, which must close command/cancel/DMA ownership before p004
  exercises the failing enumeration path.
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
- QEMU success alone does not close this Phase. The one shared BR-T34
  `usb-storage: sda` observation is the physical U2 gate.

## Expected files and subsystems

- `drivers/pci-xhci.c`
- a small shared xHCI control/context helper under `include/drivers/`
- `drivers/usb.c` and `include/drivers/usb.h` for reset completion and EP0
  packet-size handoff if the HCD interface needs it
- `plan/ws003-bringup/tests/` fixtures, runbook, and evidence
- existing WS004 xHCI/USB fixtures where a shared regression must be extended

## Ordered work packages

- [x] Add BR-T27 exact-DWORD fixtures for no-data and IN/OUT data control
      transfers, including every reserved-zero, TRT, DIR, IOC, CH, and TD Size
      field.
- [x] Correct Setup/Data/Status construction and preserve event-pointer and
      residual matching for the resulting independent TDs.
- [x] Add a bounded port-reset state helper and BR-T27 cases for success,
      disconnect, never-cleared reset, non-enabled completion, and timeout.
- [x] Decode and validate `bMaxPacketSize0`, update EP0 through the state-valid
      xHC command before the next control stage, and assert Control Average TRB
      Length 8.
- [x] Wait two 10-ms scheduler ticks after Address Device/SET_ADDRESS so the
      following descriptor request cannot observe a sub-tick recovery delay.
- [x] Add bounded enumeration-stage diagnostics carrying the numeric
      completion code and controller-owned endpoint state.
- [x] Run BR-T27, the passing p005 BR-T28/BR-T29 gates, applicable WS004
      regressions, `make -j16`, and
      `git diff --check`. Do not use `make check` or `.internal/` material.
- [x] Run the legacy-BIOS q35/xHCI USB-root control and BR-T24 OVMF USB-root at
      4, 8, and 16 GiB with one frozen production image.
- [x] Run BR-T34 once on the Latitude. Record controller/BDF, port, slot, the
      first descriptor and address/configuration stages, `usb-storage: sda`,
      and the first later stop if U2 succeeds but U3 does not; then record the
      highest physical U-tier and update P/W/M/Q evidence. Defer the final five
      consecutive boots to BR-T30.

## Acceptance cases

- `BR-T27`: host control-transfer/context/reset fixture proves the exact xHCI
  1.2 TRB words, reserved-zero contract, EP0 packet-size conversion, Average
  TRB Length, and reset-state outcomes.
- Passing p005 `BR-T28` and `BR-T29` are prerequisites, not duplicated p004
  acceptance.
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
- The p005 command/cancellation safety prerequisites remain passing.
- All declared host, QEMU, build, and formatting gates pass.
- One Latitude run reaches `usb-storage: sda`, proving U2. Repetition remains
  the final BR-T30 campaign rather than an implementation blocker.

## Actual results and evidence

Execution was authorized in q013 on 2026-08-26 after p005. The production
control/context helpers and driver path now implement the declared exact
Setup/Data/Status words, EP0 packet-size conversion, Control Average TRB
Length, bounded state-based root reset, and two-tick reset/address recovery
interval.

Automatic evidence is complete: BR-T27, prerequisite BR-T28/BR-T29, the
applicable DMA/xHCI/USB-URB/USB-storage regressions, `make -j16`, and
`git diff --check` passed. The legacy-BIOS q35/xHCI USB-only root reached
`login:`, and BR-T24 reached `login:` under OVMF at 4, 8, and 16 GiB. The
frozen 135266304-byte `build/amd64/hdd-image.img` has SHA-256
`bd3aa801ac890deabb5f0ad4b6f3388e5137992e9f6f81e8d912af4abad7585f`.

BR-T34 was then run once on the Latitude with that exact artifact. Ports 8,
10, and 13 reset and configured, and the SuperSpeed boot medium registered as
`usb-storage: sda`. UUID `45a3-2251` resolved to `/dev/sda1`. No EP0,
Control/Command, recovery, retention, enumeration, or boot-storage-timeout
error recurred. This proves U2 and completes this Phase.

The later `sense=05/20/00` stop occurred only after storage discovery, loop
attachment, and the first writable-overlay flush. It is the independent U3
SCSI cache-capability boundary extracted to
[ws003-p010](../phase010-usb-storage-flush-capability/phase.md); it does not
reopen this xHCI enumeration result.

## Interruption / resumption

No p004 work remains. Resume U3 in `ws003-p010`; repeatability remains the
later BR-T30 gate.

## Remaining debt and handoff

- If the spec-correct control path still fails on only one physical port or
  speed, parse Supported Protocol capabilities and extract protocol-specific
  root-port handling rather than accepting arbitrary Speed IDs.
- BR-T34 reached `usb-storage: sda`; its later unsupported SCSI flush is now
  owned by `ws003-p010` under the U3 BR-06 boundary.
- Multi-device concurrency and broader endpoint scheduling are tracked by the
  p005/common USB handoff unless required for the boot device.

## Reference

- [Intel xHCI Requirements Specification 1.2b](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf), especially sections 4.3.1, 4.6.9, 4.8.3, 4.11.2.2, 4.11.2.4, 6.2.3, and 6.4.1.2.

# WS003 Phase 009: xHCI SuperSpeed endpoint context

Last updated: 2026-08-26

WSID: `ws003`

Phase ID: `p009`

Combined ID: `ws003-p009`

Status: Complete (`q013`)

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md)

## Objective

Preserve the physical root-port speed ID and the SuperSpeed Endpoint Companion
maximum-burst value required to construct valid Slot and bulk Endpoint
Contexts for the Latitude boot medium.

## Confirmed defects

- Generic speed classification collapsed every raw Port Speed ID at or above
  four to one value, while the xHCI Slot Context must retain the root PORTSC
  speed ID, including PSI-defined IDs 5--15.
- Configuration parsing discarded descriptor type 48, so `bMaxBurst` was
  always zero in configured bulk Endpoint Contexts.

## Scope

- Snapshot the root PORTSC speed ID when the xHCI device is enabled and reuse
  it in every Slot Context rebuild.
- Keep the generic USB speed enum separate from that controller value.
- Parse an Endpoint Companion Descriptor into the immediately preceding
  endpoint and expose `bMaxBurst` to the HCD.
- Program Endpoint Context word 1 bits 15:8 for SuperSpeed bulk endpoints.
- Cover raw speed IDs and context word construction in BR-T38.
- Share the one final BR-T34 hardware run; do not request a separate boot.

## Non-goals

- SuperSpeedPlus bandwidth scheduling, streams, hubs, or PSI database UAPI.
- Isochronous/interrupt Mult and Max ESIT Payload; record these as later USB
  class coverage unless required by the boot medium.

## Ordered work packages

- [x] Preserve raw root-port speed ID in xHCI device state.
- [x] Parse and expose SuperSpeed Endpoint Companion `bMaxBurst`.
- [x] Build Slot/bulk Endpoint Context words from those values.
- [x] Pass BR-T38, build, hotplug, legacy, and BR-T24 gates.
- [x] Feed the shared BR-T34 result into this Phase.

## Completion conditions

- Slot Speed matches PORTSC raw Speed ID instead of a collapsed generic enum.
- SuperSpeed bulk Max Burst is represented in the Endpoint Context.
- Integrated automated gates pass and the one BR-T34 observation is recorded.

## Actual results and evidence

- xHCI snapshots the root PORTSC Speed field into `xhci_device.speed_id` and
  uses it for every Slot Context rebuild, independently of the generic USB
  speed classification. PSI-defined raw values 5--15 therefore no longer
  collapse before Slot Context construction.
- The USB configuration parser validates descriptor type 48 as the companion
  of the immediately preceding endpoint and retains `bMaxBurst`. xHCI exposes
  it through the endpoint accessor and writes it to Endpoint Context word 1
  bits 15:8 for SuperSpeed bulk endpoints.
- BR-T38 passed raw speed IDs 4, 5, and 15, zero/nonzero Max Burst Endpoint
  Context words, valid companion decode, and malformed/truncated descriptor
  rejection in the production-shared host fixtures.
- `make -j16`, applicable host regressions, `git diff --check`, BR-T29, the
  legacy BIOS q35/xHCI USB-only root gate, and BR-T24 at 4, 8, and 16 GiB
  passed with candidate SHA-256
  `bd3aa801ac890deabb5f0ad4b6f3388e5137992e9f6f81e8d912af4abad7585f`.
- The one shared BR-T34 Latitude run reset and configured the SuperSpeed port
  13 medium, registered it as `usb-storage: sda`, and completed BOT commands
  without a Slot/Endpoint Context or Max Burst failure. Together with BR-T38,
  this completes the Phase. The later valid SCSI `05/20/00` response is owned
  by `ws003-p010`.

## Remaining debt and handoff

Interrupt/isochronous companion fields, streams, and bandwidth admission remain
later WS004 USB work.

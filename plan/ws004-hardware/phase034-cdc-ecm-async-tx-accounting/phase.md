# WS004 Phase 034: CDC ECM asynchronous TX accounting

Last updated: 2026-09-01

Phase ID: `ws004-p034`

Status: planned/deferred; nonblocking; not queued

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Apply the completed q054 `net_device_tx_error()` contract to the independent
CDC ECM driver. A frame accepted by the driver keeps its existing
`tx_packets`/`tx_bytes` accounting; a genuine later terminal bulk-OUT failure
adds exactly one `tx_errors`, leaves `tx_dropped` unchanged, and an
administrative `CANCELLED` completion adds neither an error nor a drop.

This is a small consistency follow-up to completed p019 and p017. It does not
block q055 WLAN work, reopen the ECM/NCM architecture, or require physical
hardware.

## Dependencies

- `ws004-p019`: independent CDC ECM driver, production-source fixture, and
  four-cell QEMU `usb-net` baseline.
- `ws004-p017`/q054: the locked common `net_device_tx_error()` helper and the
  accepted-versus-completed TX statistics contract.
- The USB core's one terminal-claim callback and checked cancel/drain
  ownership established by p011/p015.

## Frozen accounting and ownership contract

- Successful driver acceptance remains the only increment point for
  `tx_packets` and `tx_bytes`; a later terminal result never rolls them back.
- Only `STALL`, `TIMEOUT`, `DISCONNECTED`, and `IO_ERROR` increment
  `tx_errors`, exactly once, from the sole ECM TX URB completion callback.
- Administrative `CANCELLED` during close, detach, or shutdown increments no
  failure counter. `tx_dropped` retains its existing synchronous-rejection
  meaning and is unchanged by every asynchronous terminal result.
- Callback-side publication precedes worker polling so close or detach cannot
  discard an already published genuine error. Poll/drain retains ownership of
  `tx_busy` and persistent-URB reuse exactly as in p019; this Phase adds no new
  USB or network API.

## Planned implementation and verification

1. Add the same four-status classifier used by CDC NCM to the ECM TX completion
   path and call `net_device_tx_error()` once outside the ECM lock for a
   genuine terminal failure only.
2. Extend the production-source ECM fixture with accepted packet/byte
   retention, every genuine terminal status, unchanged drops, administrative
   cancellation, completion-before-poll, completion-before-close/detach,
   repeated poll/drain, failed-drain retention, close/open reuse, and fresh
   reconnect-generation cases.
3. Run the ECM fixture in ordinary, ASan/UBSan, and compiler-analyzer modes;
   retain the q054 net-device/NCM accounting tests and USB lifecycle
   regressions; pass configured amd64/i386 and repository `make -j16` builds.
4. Rerun p019's four fresh QEMU ECM cells: IDE/static, IDE/DHCP, shared-xHCI
   USB-root/static, and shared-xHCI USB-root/DHCP. Preserve carrier, traffic,
   counter, detach, reconnect, and concurrent-storage results.

## Completion conditions

- Each accepted ECM transmit retains one packet/byte count and each genuine
  later terminal failure adds exactly one error without adding a drop.
- Administrative cancellation and repeated poll/drain paths add no error or
  drop, and no counter or busy state leaks across close, detach, or reconnect.
- Focused, sanitizer, analyzer, build, retained-regression, and four-cell QEMU
  ECM gates pass. No physical checkpoint is requested or claimed.

## Reconsideration boundary

Return to planning if ECM cannot adopt the q054 helper without changing the
common counter meanings, USB terminal-claim contract, or public UAPI. Do not
extract a shared ECM/NCM backend, add autonomous TX recovery, or turn this
nonblocking accounting follow-up into a physical campaign.

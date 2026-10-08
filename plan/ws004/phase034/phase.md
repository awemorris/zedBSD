# WS004 Phase 034: CDC ECM asynchronous TX accounting

Last updated: 2026-09-01

Phase ID: `ws004-p034`

Status: implemented, test-wait（2026-10-09 P1: code と build まで。p019 の production-source の ECM の fixture は 2026-10-03 の repository の作り直しで tree に無い。QEMU の ECM の確かめは Q1 の判断）

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

## 2026-10-09 P1（ベータ3 の合間の仕事）

- 実装（`src/drivers/usb/usb-cdc-ecm.c`）: 新しい `ecm_tx_status_is_error()`（CDC NCM の `ncm_tx_status_is_error()` と同じ 4 つ: STALL・TIMEOUT・DISCONNECTED・IO_ERROR）。`ecm_completion()` は TX の URB の完了でこれを adapter の lock の中で決め、lock の外で `net_device_tx_error()` を 1 回呼んでから poll を予約する（NCM と同じ順。close・detach の前に数え終わる）。CANCELLED は数えない。tx_packets・tx_bytes（受け付けた時に数える）と tx_dropped（同期の拒否）は変えない。新しい USB・network の API は無い。
- 確認: `make ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/p1-ws177 build/p1-ws177/vmunix`（-Werror、`usb-cdc-ecm.o` を含む、warning 0）。style-check は前より 1 件少ない（新しい指摘 0）。
- 未実施: 計画の 2・3（production-source の ECM の fixture の拡張と、ordinary・ASan/UBSan・analyzer の実行）は、fixture が tree に無いので行っていない。作るなら新しい host の fixture（ecm の source を stub の USB core と net_device で包む）が要る。QEMU の p019 の usb-net の 4 cell（`plan/ws004/tests/qemu-usb-cdc-ecm.mk`）が今の build の規則で動くかは確かめていない。

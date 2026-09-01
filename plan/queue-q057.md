# Queue: RTL8822BU minimum radio and scan

Last updated: 2026-09-01

QID: `q057`

Queue status: completed

Queue finished: **Yes**

Authorization: the user directed WLAN to remain the priority, selected
BSD-3-Clause for the RTL8822B initialization-table import, and decided that a
common RTL88 layer, RTL8822CE, and Intel AX201 are not part of the current
implementation. Q056 completed the exact USB/firmware/pre-radio substrate.

Timebox: none. Execute only the finite, automatically verifiable p028 radio and
scan milestone. No physical zedBSD radio run or human acceptance is requested
in this Queue.

Parent: [master plan](master.md)

Previous Queue: [q056](queue-q056.md)

## Purpose

Turn the tested RTL8822BU substrate into the smallest truthful station-mode
radio: import the licensed RTL8822B initialization data, start the pinned
firmware on first open, program a conservative 2.4-GHz/20-MHz profile, and feed
bounded passive/allowed-wildcard-active scan results through the p027 generic
WLAN contract. Authentication, association, keys, IP networking, broader RF
capability, and physical acceptance remain later milestones.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p028` | [Phase](ws004-hardware/phase028-rtl8822bu-usb-scan/phase.md) | automatic milestone completed | Licensed table import, minimum safe radio start, bounded software scan, and complete production-path synthetic results pass all automatic gates |

## Accepted decisions

- The production match remains only the retained Japan unit:
  `2357:012e`, `bcdDevice=2.10`, `ff/ff/ff`, endpoints
  `84/05/06/08/87`.
- Import only required RTL8822B table data under BSD-3-Clause into a dedicated
  `.inc`, retaining SPDX, Realtek copyright, full notice, immutable upstream
  commit/path, and source SHA-256. Do not copy Linux control flow.
- The initial radio is station-only, 2.4 GHz channels 1--11, 20 MHz, passive
  scan plus wildcard active probes only where the conservative world profile
  permits transmission.
- Keep the RTL8822BU implementation self-contained. Do not introduce a common
  RTL88 layer; RTL8822CE and Intel AX201 remain deferred independent targets.
- Firmware remains the separately selected, immutable
  `userland/firmware/rtl8822b/` package and is loaded from the fixed
  `/lib/firmware/rtw88/rtw8822b_fw.bin` path only on first open.

## Boundaries

- Preserve Mass Storage, CDC NCM/ECM, USB HID, wired networking, legacy HCDs,
  and the q056 lifecycle/teardown contracts.
- Keep scan timing, cache, generation, and frame-normalization policy in the
  p027 common WLAN layer; keep USB/register/table/channel work in this driver.
- Do not implement authentication, association, WPA/WEP/EAPOL, key install,
  encrypted data, DHCP, `/sbin/wifi`, `net wifi`, 5 GHz, DFS, HT/VHT, AP,
  monitor mode, throughput tuning, or same-endpoint multi-URB rings.
- Do not perform an independent physical Archer request. The later combined
  WS005 p008 checkpoint owns the first physical attach/scan/connect evidence.
- Do not use `.internal/` or aggregate `make check`.

## Automatic gates

1. Reproducibly verify the imported `.inc` source revision/path/hash and full
   BSD-3-Clause notice, and prove it contains data only.
2. Extend production-source and fake-register tests over supported/unsupported
   cut/RFE identities, checked MAC/BB/AGC/RF programming order, finite waits,
   rollback, channel 1/6/11 selection, regulatory clamping, and no 5-GHz path.
3. Prove first-open immutable firmware load, DDMA/start, table programming, RX
   arm, close/retry, error unwind, cancellation, detach, and staging scrub.
4. Drive passive and allowed wildcard-active software scans through the full
   production scan path using fake USB/register/RX inputs; verify bounded
   generations, terminal publication, malformed/stale rejection, and storage
   progress on the same fake controller.
5. Pass ordinary, ASan/UBSan, analyzer, driver-enabled amd64/i386,
   `make -j16`, disposable amd64 IDE and q35 xHCI USB-root exact-login, and
   `git diff --check` gates.

## Completion definition

Q057 completes when p028 can automatically start the pinned firmware and
minimum licensed radio model, scan channels 1--11 within the common 15-second
generation budget, and publish truthful synthetic BSS records through the
production driver/common-core path with complete failure rollback. Completion
unblocks p029 but makes no claim that physical RF has succeeded.

## Execution result

Q057 completed its finite automatic p028 milestone. The pinned MAC/BB/AGC/RF
arrays are imported as a data-only BSD-3-Clause `.inc` with exact provenance,
word-count, and no-control-flow checks. A driver-enabled binary image now
automatically carries the complete Realtek table notice under
`/usr/share/licenses/rtl8822b-tables/LICENSE`, independently of the optional
firmware package.

The production core now covers checked power-on/off and already-powered
normalization, three-bulk-OUT FIFO/RQPN/LLT/H2C setup, HS USB RX aggregation,
conditional table selection, RF SIPI, supported RFE/path programming, minimum
MAC protocol timing, 2.4-GHz 20-MHz channel 1--11 selection, and an absolute
TXAGC index-zero floor. Only a bounded 1-Mbit/s wildcard management probe is
admitted for active scan. Worst-case fixture initialization used 3,108 writes,
137 reads, and 412,326 microseconds of explicit delay under one finite
15-second open deadline.

The USB first-open path loads the immutable pinned firmware before radio/RX/
station publication, and its reverse close/detach path drains admitted work.
Channel transport failure now synchronizes the core and USB state to OFF,
clears scan/firmware/radio flags, quarantines later work, and retains ownership
until checked teardown. Passive and permitted wildcard-active production scans
passed channels 1--11, terminal generations, stale/malformed inputs, fault
injection, retry, close, shutdown, detach, and concurrent fake-storage cases.

Table, core, USB, and firmware-package focused runners passed ordinary,
ASan/UBSan, and analyzer variants where supplied. Forced driver-enabled amd64
and i386 builds passed with both production symbols present; the amd64 rootfs
contained the exact table license. Ordinary `make -j16`, disposable amd64
IDE-root exact login, and q35/xHCI USB-root exact login passed. The source image
was not mutated, `git diff --check` passed, and an independent read-only audit
found no cold-open/passive/active scan P0/P1 blocker.

The possible warm-rebind RPWM `0xfe58` refinement is recorded in p030. Q057
makes no physical RF claim and does not request human work; p029 is now
dependency-ready. The single later WS005 p008 checkpoint remains the source of
physical attach/scan feedback.

Next Queue: [q058](queue.md)

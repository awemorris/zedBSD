# Queue: RTL8822BU pre-radio substrate

Last updated: 2026-09-01

QID: `q056`

Queue status: completed

Queue finished: **Yes**

Authorization: the user directed WLAN to remain the implementation priority,
approved the per-device GitHub-fetched `userland/firmware/` boundary, and
selected BSD-3-Clause for the later RTL8822B initialization-table import. Q055
completed the required generic WLAN common layer.

Timebox: none. Complete the one finite, automatically verifiable p036
pre-radio Phase. No physical radio action or PCI passthrough is required.

Parent: [master plan](master.md)

Previous Queue: [q055](queue-q055.md)

## Purpose

Build the independently testable RTL8822BU substrate before programming RF:
the default-off firmware package, immutable firmware validation, exact Archer
USB/register/efuse contract, reserved-page/DDMA model, RX aggregate parser,
endpoint lifetime, serialized net-device/WLAN attachment, and synthetic p027
scan delivery. This keeps the next p028 radio/table Queue finite and prevents
physical RF debugging from carrying unrelated package, parser, and ownership
uncertainty.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p036` | [Phase](ws004-hardware/phase036-rtl8822bu-pre-radio-substrate/phase.md) | completed | Optional pinned firmware package plus exact USB/board/firmware/RX/lifecycle substrate passes every automatic gate while production refuses tableless radio start |

## Accepted decisions

- Exact match remains the retained Japan unit: `2357:012e`,
  `bcdDevice=2.10`, `ff/ff/ff`, endpoints `84/05/06/08/87`.
- Kernel/base images embed no Realtek firmware. Only explicit selection of the
  default-off `userland/firmware/rtl8822b/` entry may fetch the pinned GitHub
  mirror and install verified firmware plus notices.
- P036 imports no PHY table. Production open returns `EOPNOTSUPP` before
  firmware upload or RF writes; fake transport tests own upload/RX evidence.
- P028 will import the RTL8822B table as a dedicated BSD-3-Clause `.inc` with
  Realtek notice, full license, immutable source commit/path/hash, and no Linux
  control-flow copy.
- Do not create a common RTL88 chip layer now. PCI RTL8822CE/8822C and Intel
  AX201 are later independent drivers with their own future firmware entries;
  only p027 and later proven device-independent 802.11 behavior are reusable.

## Boundaries

- Keep firmware acquisition selected-only and ordinary builds fully offline.
- Keep all firmware, SHA, efuse, descriptor, and RX helpers private to the
  RTL8822B module; do not add a casual public kernel API or user UAPI.
- Use the p027 LIVE-publication then removal-serialized station-attach contract
  and exact reverse checked unwind.
- Preserve Mass Storage, CDC NCM/ECM, USB HID, wired networking, and every
  legacy-HCD behavior.
- Do not add RF tables, channel programming, probe transmission, real scan,
  WPA, DHCP, `/sbin/wifi`, or `net wifi` in q056.
- Do not use `.internal/` or aggregate `make check`.

## Automatic gates

1. Package tests prove default-off/no-network behavior, exact pinned fetch and
   hashes, license/manifest installation, offline verified-cache reuse, corrupt
   cache rejection, and no partial output.
2. Production-source pure tests cover SHA vectors, firmware format/digest/
   segments, efuse sparse decode and RFE/MAC bounds, RX aggregate/C2H/error
   parsing, and synthetic beacon delivery through p027.
3. Fake USB/register tests cover exact/neighbor match, endpoint topology,
   control width/endian/short/timeout, reserved-page/DDMA/FW-ready, single-URB
   poll/rearm, cancellation, unplug/retry, allocation failures, and reverse
   unwind while fake storage progresses.
4. Lifecycle tests cover the not-ready callback gate, LIVE create then
   serialized station attach, open/close/ioctl/poll against detach/shutdown,
   reference balance, stable `wlanN`, and tableless `EOPNOTSUPP`/carrier-down.
5. Ordinary, ASan/UBSan, analyzer, driver-enabled amd64/i386, `make -j16`,
   disposable amd64 IDE and q35 xHCI USB-root exact-login, and
   `git diff --check` gates pass.

## Completion definition

Q056 completes when p036's selected-only firmware package and all pre-radio
production/fake components pass automatically, ordinary images remain
firmware-free and offline, and a real configured driver cannot start a radio
without p028's licensed table. P028 then becomes the next WLAN Queue with no
remaining table-license decision. Physical Archer and RTL8822CE observations
remain later, separately identified evidence.

## Execution result

Q056 completed its single finite Phase. The new default-off
`rtl8822b-firmware` entry fetches only the immutable GitHub revision selected
by p026, verifies the exact firmware/license/WHENCE hashes, installs the blob,
notice, and package manifest atomically, reuses only a verified offline cache,
and leaves an ordinary image blob-free. Production pin values cannot be
overridden from the build command line; variable fixture inputs are isolated
to one test-only goal. Menuconfig exposes exactly Base, X11, Firmware, and
Packages, and selecting a kernel driver alone neither downloads nor stages
firmware.

The RTL8822B production core now supplies an immutable VFS loader, SHA-256 and
firmware/segment validation, bounded efuse decoding, checked TX/RX/C2H codecs,
and synthetic scan delivery. The exact `2357:012e` USB driver validates the
retained five-endpoint topology, register transport, firmware/DDMA model,
single persistent RX request, and serialized net-device/WLAN publication and
removal. Open/close, shutdown, detach, ioctl, poll, allocation-failure unwind,
and request cancellation were exercised under deterministic races. A
production open still returns `EOPNOTSUPP` before firmware upload or RF writes,
so q056 cannot accidentally claim a tableless radio.

The firmware package, RTL8822B core, USB-driver, UFS1 inline-symlink/
single-indirect, and menuconfig tests passed, including ASan/UBSan and analyzer
modes where provided and five consecutive USB-driver runs. Forced driver-
enabled amd64 and i386 kernels and ordinary `make -j16` passed. Disposable
amd64 IDE and q35 xHCI USB-root boots each reached exact `login:` once. A
selected UFS contained the exact 161,240-byte firmware with its frozen SHA-256;
the same UFS rebuilt with the entry deselected contained no firmware. Two
independent focused audits reported no remaining P2/P3 issue, and
`git diff --check` passed. P028 is therefore dependency-ready for q057; no
physical-radio claim was made.

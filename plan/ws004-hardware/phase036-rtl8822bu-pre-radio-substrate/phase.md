# WS004 Phase 036: RTL8822BU pre-radio substrate

Last updated: 2026-09-01

Phase ID: `ws004-p036`

Status: complete (`q056`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Implement every RTL8822BU component that can be proved before programming the
production MAC/BB/AGC/RF/RFE tables: the optional firmware package, immutable
firmware reader and validator, exact Archer USB binding, bounded register
transport, efuse decoding, reserved-page/DDMA firmware model, RX aggregate
parser, endpoint lifetime, net-device/WLAN-station attachment, and synthetic
scan delivery. This Phase deliberately stops before a production RF write or
real channel operation; the user-approved BSD-3-Clause table import and actual
radio scan remain in `ws004-p028`.

The split is an implementation boundary, not a reduction of the WLAN goal.
P036 must complete automatically so p028 begins with a tested bus, firmware,
parser, ownership, and common-core substrate rather than debugging all layers
on physical RF at once.

## Dependencies

- `ws004-p010`, p011, and p015: exact USB descriptors, interface binding,
  checked control/endpoint ownership, concurrent endpoint requests, and
  disconnect drain.
- `ws004-p012`: removable net-device publication and teardown barriers.
- `ws004-p026`: exact `2357:012e` identity and pinned firmware/license/hash
  policy.
- `ws004-p027`: completed generic WLAN UAPI, station state, scan cache,
  generation, timer, and lifetime contract.

## Optional firmware package

Create the top-level `userland/firmware/` source category and add
`userland/firmware/rtl8822b/` as a selectable, default-off, data-only firmware
entry for i386 PC/AT, i386 PC-98, and amd64. The menuconfig userland view is divided into `Base`,
`X11`, `Firmware`, and `Packages`; this entry appears only under `Firmware`.
Merely listing entries, opening menuconfig, selecting the kernel driver, or
building an ordinary image must perform no network access and must not create
a firmware source cache. Only explicit selection of `rtl8822b-firmware` may
acquire the unmodified files from the p026-pinned `endlessm/linux-firmware`
GitHub revision.

The package verifies all frozen hashes before atomic staging and installs:

- `/lib/firmware/rtw88/rtw8822b_fw.bin`;
- `/usr/share/licenses/rtl8822b-firmware/LICENCE.rtlwifi_firmware.txt`; and
- `/usr/share/zedbsd/packages/rtl8822b-firmware.manifest`.

The manifest records mirror and official revisions, upstream paths, version,
sizes, SHA-256 values, and redistribution notice. No blob is committed to the
zedBSD source tree. A verified cache may be reused offline; a missing or
corrupt selected cache fails visibly and never installs partial output.

## Driver-private immutable firmware input

The RTL8822B implementation reads only the fixed absolute firmware path through
the existing kernel VFS and `file_content_lease` snapshot contract. Keep this
loader private to the chip module in this Phase rather than adding a casual
public firmware API. It accepts only a regular file of the exact frozen size,
rejects short reads, size changes, trailing data, malformed headers, wrong
version/segments, and wrong SHA-256, and returns ownership only after complete
validation. Every release scrubs and frees the buffer. There is no kernel
network fetch, fallback blob, user pointer, or path override.

The private SHA-256 implementation is tested with standard public vectors but
is not exported as a general crypto interface.

This Phase does not create placeholder downloads for
`userland/firmware/rtl8822c/` or `userland/firmware/intelax211/`. Those paths
belong to the deferred, separate RTL8822CE and exact AX211 driver efforts.
There is no common RTL88 chip layer in this milestone; the 8822B/USB module is
allowed to stand alone below the stable p027 WLAN interface.

## Exact USB and board substrate

The driver configuration is `CONFIG_DRIVER_USB_RTL8822BU`, default `n`, on
i386/amd64. The production match accepts only the retained unit contract:

- VID:PID `2357:012e`, `bcdDevice=2.10`;
- interface class/subclass/protocol `ff/ff/ff`; and
- unique endpoints `0x84` bulk IN, `0x05`, `0x06`, `0x08` bulk OUT, and
  `0x87` interrupt IN with the retained packet/topology constraints.

Endpoint `0x87` is topology evidence only and is not armed in this milestone.
Do not switch the observed High-Speed device into an unproven USB-3 mode.

Register reads/writes use vendor request `0x05`, read type `0xc0`, write type
`0x40`, `wValue=register`, `wIndex=0`, exact 1/2/4-byte little-endian payloads,
and finite deadlines. A short control transfer, timeout, stall, or disconnect
is an error. Efuse handling is bounded to physical 1024 bytes, logical 768
bytes, protected tail 96 bytes, checked sparse-map decoding, USB MAC offset
`0x107`, and supported RFE values 2/3/5. The access grant is removed on every
exit. Invalid MAC, bounds, cut, or RFE prevents publication; the driver does
not fabricate board data or NSS/bandwidth capability.

## Firmware and receive model

Pure production codecs validate the 161,240-byte firmware, 64-byte header,
11,208-byte DMEM plus 8-byte checksum, and 149,952-byte IMEM plus 8-byte
checksum. A fake register/USB transport proves the WCPU-3081 reserved-page
flow: checked 48-byte TX descriptor, endpoint `0x05`, internal DDMA chunks no
larger than `0x1000`, checksum completion, and finite `FW_READY`. This Phase
does not use the legacy vendor-control firmware page writer.

The receive codec validates the 24-byte descriptor, `shift`, `drv_info * 8`,
packet length, 8-byte aggregate alignment, CRC/ICV status, FCS removal, C2H
separation, and bounded PHY/RSSI metadata. Only validated beacon and probe-
response frames reach `wlan_station_report_scan_frame()` in poll/thread
context. USB completion only latches bounded ownership and schedules poll.
One persistent 32-KiB request is used on endpoint `0x84`; poll drains and
rearms it, while close/detach cancels and joins it. P035 remains the optional
future same-endpoint multi-URB extension.

## Publication and rollback ordering

The exact driver order is:

1. provisionally bind the USB interface and allocate driver/endpoint resources;
2. read and validate board identity, allocate a `net_device`, and leave the
   driver callback gate not-ready;
3. hold the driver lifecycle lock, call `net_device_create()` to make it LIVE,
   then immediately call `wlan_station_attach()`;
4. publish the station pointer and ready gate only after attach succeeds; and
5. commit the USB binding transaction last.

The same lifecycle lock serializes USB detach and the net-device close callback
against the complete live-publication/attach interval. Callbacks admitted in
the short not-ready interval return `ENODEV` without touching partial state. An
attach failure runs checked `net_device_gone()` before destroy. Detach first
makes the net device gone and joins callbacks, then successfully detaches the
station (which releases its own device reference), destroys the net device,
and finally frees URBs and `radio_context`. Static radio ops and context remain
valid until station detach succeeds.

Without p028's approved table file, a production open returns explicit
`EOPNOTSUPP` before firmware upload or any radio write and keeps carrier down.
The fake transport is allowed to exercise upload and RX codecs; this is not a
claim that the Archer radio has started.

## Verification gates

1. A package fixture proves default-off/no-network behavior, exact selection,
   pinned revision/hash/license/manifest, verified offline cache reuse, corrupt
   cache rejection, and absence of partial installation.
2. Production-source codec fixtures pass SHA-256 vectors, firmware size/header/
   digest/segment negatives, efuse sparse-map and board negatives, RX aggregate
   bounds/C2H/error cases, and synthetic beacon delivery to the p027 cache.
3. A fake USB/register model passes exact/neighbor identity, endpoint topology,
   register endian/width/short/timeout, reserved-page/DDMA/FW-ready success and
   failures, one-URB poll/rearm, cancellation, unplug, retry, and allocation-
   failure reverse unwind.
4. Lifecycle tests race open/close/ioctl/poll with detach/shutdown and verify the
   not-ready gate, LIVE-publication then serialized station attach, balanced
   references, stable `wlanN`, and concurrent fake USB storage progress.
5. Ordinary, ASan/UBSan, and analyzer modes pass. Forced driver-enabled amd64
   and i386 builds, ordinary `make -j16`, and disposable amd64 IDE plus q35
   xHCI USB-root boots reach exact `login:`. `git diff --check` passes.

Do not use `.internal/` or aggregate `make check`.

## Execution evidence (`q056`)

Q056 completed this Phase on 2026-09-01. The three Phase-owned runners passed:

```sh
plan/ws004-hardware/tests/run-rtl8822b-firmware-package-test.sh
RTL8822B_FIRMWARE_TEST_BLOB=build/sources/firmware/rtl8822b/2f56219d20e4becccd718963fc3bcc671c543ce5/rtw8822b_fw.bin \
  plan/ws004-hardware/tests/run-rtl8822b-core-test.sh
plan/ws004-hardware/tests/run-usb-rtl8822bu-driver-test.sh
```

The package matrix passed default-off/no-fetch discovery, explicitly selected
pinned acquisition, immutable metadata, verified offline reuse, corruption and
short-input rejection, and atomic no-partial-output cases. The core runner used
the real selected firmware blob and passed its ordinary, ASan/UBSan, GCC
analyzer, immutable-VFS loader, firmware/header/segment, SHA-256, efuse/board,
TX-descriptor, DDMA, and RX aggregate cases. The USB runner passed ordinary,
ASan/UBSan, and analyzer modes, including exact descriptor/register transport,
FW-ready rejection, attach-failure reverse unwind, checked detach retry,
open/close/shutdown/ioctl/poll joining, and deterministic RX start/stop/detach
races with a real fixture spin lock. All required focused regressions and
`git diff --check` passed.

Forced driver-enabled amd64 and i386 production object builds passed, followed
by the ordinary `make -j16` repository build. Disposable runtime controls
passed 1/1 for amd64 IDE root and 1/1 for q35/xHCI USB root, each reaching the
exact `login:` gate. Separate UFS image inspection proved that an explicit
real-firmware selection installed the pinned firmware, license, and manifest,
while the otherwise identical non-selected image contained none of those
files; the ordinary image path remained offline.

A final independent read-only audit found no remaining correctness or lifetime
blocker. In particular, RX submit ownership now remains live through the last
URB status/cancel access, teardown waits for admitted start and poll work,
FW-ready masks every non-clock status bit, and the concurrent callback/detach
fixtures use real mutual exclusion.

The production boundary was also exercised directly: the exact device can be
published as `wlan0`, but `open` returns `EOPNOTSUPP` with carrier down and with
zero firmware bulk transfers or register-control writes. Thus these results
complete the pre-radio substrate only; they do not claim firmware execution,
RF initialization, physical Archer operation, or scan success. Those remain
owned by p028.

## Completion conditions

- Explicit package selection installs only the pinned, verified firmware and
  notices; ordinary builds remain offline and blob-free.
- The exact USB/board match, register transport, efuse, firmware/DDMA/RX pure
  components, endpoint lifetime, and p027 integration pass every automatic
  gate without importing an RF table.
- Production cannot accidentally start a tableless radio and reports
  `EOPNOTSUPP` honestly while carrier stays down.
- P028 can add the selected BSD-3-Clause `.inc` table and real channel/scan path
  without changing the frozen p027 or p036 ownership interfaces.

## Reconsideration boundary

Stop only the affected branch if the retained descriptor is contradicted, the
firmware format does not match the frozen evidence, RFE is outside 2/3/5, VFS
cannot supply an immutable snapshot, or the existing USB teardown contract
cannot join the request graph. Preserve all independently passing package,
codec, and ownership work and record the exact boundary. Do not broaden the
USB match, upload unverified bytes, embed firmware, or perform speculative RF
writes.

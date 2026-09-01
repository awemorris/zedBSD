# WS004 Phase 028: RTL8822BU USB attach, firmware, and scan

Last updated: 2026-09-01

Phase ID: `ws004-p028`

Status: planned; `ws004-p026` complete; depends on completion of
`ws004-p027`; not queued

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Bind the descriptor-confirmed Archer T3U Nano as an RTL8822BU USB WLAN device,
load the separately packaged and pinned 8822B firmware, initialize the minimum
safe station-mode radio profile, and prepare truthful scan results through the
generic WLAN UAPI. It stops before authentication, association, key
installation, encrypted data, DHCP, or 5-GHz operation. Its automatic
implementation may feed p029/p030 without an independent hardware run; the
first zedBSD radio observation is the later combined WS005 p008 checkpoint.

## Dependencies

- `ws004-p010`, `p011`, and `p015`: retained USB descriptors, concurrent xHCI
  URBs, interface-scoped claims, callback drain, checked alternate/configuration
  rollback, and detach ownership.
- `ws004-p012`: removable `net_device` and carrier lifetime.
- `ws004-p026`: the Japan-market unit has no printed revision; its exact
  `2357:012e`, `bcdDevice=2.10`, `ff/ff/ff`, five-endpoint descriptor is
  authoritative, and one firmware blob, digest, license, optional-package
  acquisition source, and install path are frozen.
- `ws004-p027`: generic WLAN UAPI, scan cache, operation generations, common
  state, and fake-driver contract.

The exact-device and firmware-package decisions in `p026` are complete. The
remaining Queue prerequisite is successful completion of `p027`'s common scan
state machine, lifetime contract, and automatic gates. It is not added to the
current implementation Queue by this plan.

## Frozen initial radio profile

The first usable profile is deliberately smaller than the adapter's marketed
capability:

- station mode only;
- 2.4-GHz channels 1--11, 20-MHz width, legacy rates sufficient for management
  and basic data preparation;
- the board/efuse regulatory limits clamped to a conservative world profile,
  with no user power or country override;
- passive beacon collection plus active wildcard probe only on channels where
  that conservative profile permits transmission;
- no 5 GHz, DFS/radar handling, 40-MHz HT, VHT, beamforming, monitor/AP mode,
  power-save optimization, Bluetooth coexistence tuning, aggregation, or rate
  optimization; and
- no claim of full AC1300 performance.

The acceptance AP uses channel 1, 6, or 11 and a 20-MHz BSS. 5 GHz, DFS, HT/
VHT optimization, and broader regulatory support require later Phases with
their own evidence.

## Exact binding and publication contract

1. Match only the `idVendor`, `idProduct`, interface class/subclass/protocol,
   and endpoint topology recorded by `p026`. The initial table contains only
   TP-Link `2357:012e`; the product name and a generic Realtek vendor interface
   never match by themselves.
2. Bind the matching primary interface through the USB core's provisional
   binding transaction. The device has one interface, so do not self-claim it
   through the sibling-interface claim API. Do not disturb another interface
   or choose an alternate/configuration not present in the retained physical
   descriptor.
3. Validate all bulk endpoint directions, packet sizes, burst metadata, and
   address uniqueness before allocating requests. The actual descriptor decides
   the endpoint map; Linux constants are a comparison, not a substitute.
4. Acquire in one rollback ledger: provisional primary binding, driver object,
   lifecycle/control lock, RX/TX request pools and DMA, board data, an
   allocated `net_device`, live net-device publication, and the common WLAN
   station. Firmware staging is not an attach resource; it belongs to the
   first-open transaction below. A failed step unwinds in exact reverse order.
5. `wlan_station_attach()` accepts only a live device and its retained
   reference does not pin the device in the LIVE state. The driver therefore
   keeps its lifecycle lock across `net_device_create()` and the complete WLAN
   attach, exposes a not-ready gate from every net-device callback during that
   short interval, and serializes USB detach plus the net-device close callback
   through the same lock. If WLAN attach fails, call the checked
   `net_device_gone()` barrier before destroying the net device. If removal
   wins after publication, close waits for the attach interval and then
   detaches the completed station; it must not leave an orphan station on a
   GONE device.
6. Read and validate the 8822BU efuse/board data needed for MAC address, cut,
   RFE option, RF paths, crystal calibration, channel plan, and power limits.
   An invalid MAC, unsupported cut/RFE, truncated map, or contradictory country
   data is a hard attach/open error; do not invent calibration or a random MAC.
7. Publish the first common WLAN interface as `wlan0` and later instances as
   the lowest available `wlanN`. Publication means the descriptor and board
   identity are valid, not that firmware or carrier is active.

The interface remains administratively down after publication. Its first open
loads and starts firmware; `wifi ... search start` on a down interface returns
`ENETDOWN`, while WS005 `net wifi up` owns the later open/orchestration step.

## USB transport and firmware start

The chip driver owns only RTL8822BU and USB-specific work:

- bounded vendor-control register reads/writes with explicit widths and endian
  conversion;
- the 8822B power-on/off sequence, efuse access, MAC/BB/RF table programming,
  USB endpoint/queue mapping, TX/RX descriptors, firmware commands/events, and
  key-CAM hooks used by `p029`;
- one persistent bulk-IN request on endpoint `0x84` for this scan-only
  milestone, plus bounded bulk-OUT ownership on the independently schedulable
  OUT endpoints using the concurrent-URB contract from p011; and
- translation of validated RX descriptors, firmware events, TX status, RSSI,
  channel, and security metadata into generic WLAN callbacks.

The retained target is the observed High-Speed device. The initial driver does
not perform an unproven USB-3 mode switch. Endpoint `0x87` is required by the
exact topology and is validated at attach, but this scan milestone does not arm
the interrupt endpoint; firmware events needed here are consumed through the
documented receive path.

P011 permits one active request per endpoint, not multiple requests on one
endpoint. The bulk-IN completion therefore only latches bounded work and
schedules net-device polling. Poll context drains the completed request,
validates and copies one bounded RX unit, reports it to the WLAN common core,
and then rearms the same request. Stop, close, and detach cancel and drain that
request before releasing its buffer. A second simultaneous request on `0x84`
must remain `EBUSY`; same-endpoint multi-URB rings are the separate deferred
[`ws004-p035`](../phase035-usb-same-endpoint-multi-urb/phase.md) and do not
block this Phase.

The driver and kernel never acquire firmware from a network. Only explicit
selection/build of `userland/packages/wifi-firmware/` fetches the unmodified
blob and root `LICENCE.rtlwifi_firmware.txt` from
`https://github.com/endlessm/linux-firmware.git` at
immutable revision `2f56219d20e4becccd718963fc3bcc671c543ce5`; the package
verifies the frozen blob and license SHA-256 values and stages them as an
opt-in data package for image or `DESTDIR` installation. The current package
model does not imply an on-device package database or runtime uninstall. The
package also installs a package-owned immutable manifest under
`/usr/share/zedbsd/packages/` recording the mirror revision, official
provenance, paths, sizes, and hashes. Official provenance remains
`linux-firmware` commit
`458e40fdbb4dad5134ec230a42df21aea1b5baf8`: firmware version 30.20.0, size
161,240 bytes, blob SHA-256
`a72da690597bfa99d8eb6fc2ab090d18d8ad92ac2befd35db1c9e3662d8d8418`, and
license SHA-256
`a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e`;
the official WHENCE SHA-256 is
`34f954c7d068ec4fd5fcc216471912dd3cf40ff60a7ffa8d06ff6f9b5999551f`.

On first open, request only
`/lib/firmware/rtw88/rtw8822b_fw.bin`. Validate the pinned size and SHA-256 and
the supported 8822B firmware header before any device upload. The frozen image
is 161,240 bytes with a 64-byte header, 11,208-byte DMEM plus its 8-byte
checksum, and 149,952-byte IMEM plus its 8-byte checksum. Stage immutable bytes
and use the RTL8822B/WCPU-3081 reserved-page protocol: prepend the checked
48-byte TX descriptor, submit bounded firmware pages through bulk OUT endpoint
`0x05`, and use the internal DDMA engine in chunks no larger than `0x1000`.
Do not substitute the legacy vendor-control firmware-page writer used by other
chip generations. Check every address and length operation and wait with finite
deadlines for DDMA/checksum completion and WLAN CPU ready. Only then program
the minimum radio/MAC receive path and arm RX requests.

Attach reads SYS_CFG1 plus the physical/logical efuse needed to establish the
MAC and board identity; it does not require the firmware C2H hardware-feature
query and does not guess NSS or bandwidth capability. The initial capability
surface remains the frozen 2.4-GHz/20-MHz profile until later evidence expands
it.

Missing firmware, wrong digest, unsupported header/version, short USB write,
firmware-ready timeout, firmware error event, and RX-arm failure are distinct
errors. Each leaves carrier down, cancels/drains every admitted request,
powers the device off when possible, scrubs/frees staging memory, and permits a
later checked `IFF_UP` retry. Firmware open, size/digest/header validation,
upload, radio start, and RX arming all complete before calling
`wlan_station_open()`; otherwise generic driver-open failure could leave only
the common station administratively up. There is no kernel runtime network
fetch, embedded fallback, or success after a partial start.

## Scan implementation

Scanning is a common-core operation, not a driver-owned autonomous policy. The
first implementation uses bounded software scan so correctness does not depend
on optional firmware scan offload. One scan generation has the p002/p027
15-second total monotonic deadline; every channel dwell, firmware command, USB
completion, cancellation, and terminal publication is bounded by the smaller
of its local limit and the remaining generation time:

1. The common core starts a generation and supplies the legal channel list.
2. The driver sets one 20-MHz channel, acknowledges completion, and enables
   management reception. The common timer owns dwell and cancellation.
3. Where active probing is allowed, the common core builds a standards-bounded
   wildcard probe request and the driver transmits it. Passive beacon reception
   remains sufficient for a result.
4. The driver validates the RTL RX descriptor and exact frame length, strips
   only documented descriptor/FCS bytes, and reports beacon/probe-response
   frames plus channel/RSSI metadata. The common parser validates 802.11 and IE
   bounds, deduplicates BSSID, and normalizes security.
5. Stop, timeout, USB error, close, or detach cancels the current dwell and all
   scan TX, drains callbacks, publishes one terminal generation state, and
   ignores later RX/events tagged with that generation.

A scan result does not imply that authentication or encryption is supported.
Open, WEP, WPA/TKIP, WPA3, enterprise, malformed, and PMF-required networks may
be listed with truthful capability flags; `p029` connects only to its declared
WPA2-Personal/CCMP subset.

## Host and model verification

Extend `HW-T31` with production-source fixtures for:

- exact and neighboring USB IDs, wrong interface tuples, missing/duplicate/
  malformed endpoints, allocation failure at every attach step, and exact
  reverse unwind;
- vendor-register width/endian/timeout/error handling and checked power
  sequence rollback;
- valid and invalid efuse maps, cut/RFE options, MAC addresses, channel plans,
  and conservative power/channel intersection;
- firmware header, size, digest, download-page boundaries, short transfers,
  ready/error/timeout events, retry, close, detach, and staging-buffer release;
- optional-package-only acquisition, exact immutable mirror revision, frozen
  blob/license hashes, no ordinary-base-build or kernel fetch, and separate
  installation at the fixed path;
- RX/TX descriptor bounds, foreign/truncated/oversized frames, malformed
  firmware events, and stale scan generations; and
- single-URB `0x84` completion-to-poll drain/rearm, rejection of a simultaneous
  second `0x84` request under the existing per-endpoint admission contract,
  and complete cancel/drain on every stop/unwind path; and
- scan start/stop/timeout over a fake USB transport while storage requests are
  concurrently active on the same xHCI controller.

The fixture may derive expected register/descriptor values from independently
reviewed public specifications, descriptors, traces, and Realtek/Linux
behavioral references. Register transport, descriptor parsing, firmware
loading, efuse decoding, DDMA, endpoint lifecycle, and synthetic scan glue are
implemented independently against frozen interfaces and test vectors. Linux
mainline's 8822B and USB files are behavioral references, not an implementation
source:

- <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/realtek/rtw88/rtw8822bu.c>
- <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/realtek/rtw88/rtw8822b.c>
- <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/realtek/rtw88/usb.c>

Production RF initialization has a separate provenance checkpoint. The large
RTL8822B MAC/BB/AGC/RF/RFE tables are needed even for passive reception; the
driver must not pretend that firmware alone initializes the receive path. The
public Linux table is dual-licensed `GPL-2.0 OR BSD-3-Clause`, while an
independent permissive implementation or public register guide was not found.
Before adding those tables, human review selects either a notice-preserving
BSD-3-Clause import or a clean-room transaction trace. This decision does not
block the firmware package, exact USB binding, register transport, efuse and
firmware parsers, DDMA model, RX aggregate parser, endpoint lifecycle, or
synthetic common-core scan integration.

## Physical-evidence handoff

p028 makes no zedBSD physical request. Its only hardware prerequisite is the
single read-only development-host descriptor inventory already owned by p026;
that inventory authorizes the exact match but does not prove this driver.
Missing/wrong firmware, endpoint faults, scan generations, repeated scans, and
unplug races are maximized in automatic fake-USB/model tests here.

After p028--p030 and every WS005 command/orchestration automatic gate pass on
one candidate, the first zedBSD run occurs in the one combined provisional
checkpoint owned by
[`ws005-p008`](../../ws005-networking/phase008-archer-physical-acceptance/phase.md).
That shared script retains the actual firmware/cut/RFE/endpoint diagnostics and
one controlled scan before continuing to association, lifecycle, DHCP, and
cleanup. p028 references that evidence for its physical claim; it does not ask
the user to repeat an attach or scan.

## Automatic milestone and later physical feedback

- Production binding logic accepts only the descriptor-confirmed `2357:012e`
  tuple and the fake-USB fixture publishes one stable `wlanN` identity.
- The `wifi-firmware` package-only acquisition checks and firmware
  transport/model start only the separately installed, pinned 8822B image with
  truthful provenance/version/digest diagnostics; every negative case fails
  atomically.
- Synthetic 2.4-GHz/20-MHz beacons/probe responses produce generation-
  consistent BSS records and bounded stop through the complete production
  scan path.
- Host ordinary/sanitizer/analyzer gates, amd64 and i386 configured builds,
  `make -j16`, and IDE/xHCI USB-root regressions pass.

Those conditions are the p028 automatic milestone and are sufficient for p029
to begin; they do not claim radio success. After all p028--p030 and WS005
automatic gates pass, the single p030/WS005 p008 ledger must confirm the real
attach, firmware start, and controlled scan before WS004 records physical
completion. p028 makes no independent user request and is not a physical
dependency of p029/p030, avoiding a cycle through p008.

## Explicit exclusions

Authentication, association, passphrase derivation, EAPOL, PTK/GTK, CCMP,
Ethernet data, DHCP, auto-connect, rekey, long-term reconnect, 5 GHz, DFS, HT/
VHT, aggregation, throughput tuning, AP/monitor mode, and the Latitude's
RTL8822CE are outside this Phase.

## Reconsideration boundary

Stop the production-radio portion if the adapter descriptor differs from
`p026`, efuse requires an unknown RFE/calibration table, the pinned blob is
incompatible, legal 2.4-GHz transmit limits cannot be derived conservatively,
firmware scan offload is mandatory, the PHY-table provenance decision above
is still pending, or correct USB teardown requires changing p011/p015
ownership. Continue every independently testable pre-radio item before
recording that boundary; do not broaden the USB match, claim a tableless radio,
or enable unrestricted channels as a workaround.

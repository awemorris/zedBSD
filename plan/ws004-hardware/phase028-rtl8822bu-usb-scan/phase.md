# WS004 Phase 028: RTL8822BU USB attach, firmware, and scan

Last updated: 2026-08-30

Phase ID: `ws004-p028`

Status: planned; depends on `ws004-p026` and `ws004-p027`; not queued

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
- `ws004-p026`: the exact unit is confirmed as `2357:012e`, and one firmware
  blob, digest, license, package, and install path are frozen.
- `ws004-p027`: generic WLAN UAPI, scan cache, operation generations, common
  state, and fake-driver contract.

The Phase is ineligible for a Queue proposal until the physical descriptor and
firmware-package decision in `p026` are complete. It is not added to the
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
2. Claim exactly the matching USB interface through the p015 transaction. Do
   not disturb another interface or choose an alternate/configuration not
   present in the retained physical descriptor.
3. Validate all bulk endpoint directions, packet sizes, burst metadata, and
   address uniqueness before allocating requests. The actual descriptor decides
   the endpoint map; Linux constants are a comparison, not a substitute.
4. Acquire in one rollback ledger: USB claim, driver object, control lock,
   RX/TX request pools and DMA, firmware staging buffer, common WLAN station,
   and net-device publication. A failed step unwinds in exact reverse order.
5. Read and validate the 8822BU efuse/board data needed for MAC address, cut,
   RFE option, RF paths, crystal calibration, channel plan, and power limits.
   An invalid MAC, unsupported cut/RFE, truncated map, or contradictory country
   data is a hard attach/open error; do not invent calibration or a random MAC.
6. Publish the first common WLAN interface as `wlan0` and later instances as
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
- multiple persistent bulk-IN requests and bounded bulk-OUT ownership using the
  concurrent-URB contract from p011; and
- translation of validated RX descriptors, firmware events, TX status, RSSI,
  channel, and security metadata into generic WLAN callbacks.

On first open, request only
`/lib/firmware/rtw88/rtw8822b_fw.bin`. Validate the `p026` size and SHA-256 and
the supported 8822B firmware header before any device upload. Stage immutable
bytes, split every download write within the documented page/window limit,
check all address and length arithmetic, and wait with finite deadlines for
checksum/download completion and WLAN CPU ready. Only then program the minimum
radio/MAC receive path and arm RX requests.

Missing firmware, wrong digest, unsupported header/version, short USB write,
firmware-ready timeout, firmware error event, and RX-arm failure are distinct
errors. Each leaves carrier down, cancels/drains every admitted request,
powers the device off when possible, scrubs/frees staging memory, and permits a
later checked `IFF_UP` retry. There is no runtime download, embedded fallback,
or success after a partial start.

## Scan implementation

Scanning is a common-core operation, not a driver-owned autonomous policy. The
first implementation uses bounded software scan so correctness does not depend
on optional firmware scan offload:

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
- RX/TX descriptor bounds, foreign/truncated/oversized frames, malformed
  firmware events, and stale scan generations; and
- scan start/stop/timeout over a fake USB transport while storage requests are
  concurrently active on the same xHCI controller.

The fixture may derive expected register/descriptor values from independently
reviewed public specifications, descriptors, traces, and Realtek/Linux
behavioral references. No external driver implementation is copied into the
zedBSD base: source is written independently against the frozen interfaces and
test vectors. Linux mainline's 8822B and USB files are behavioral references,
not an implementation source:

- <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/realtek/rtw88/rtw8822bu.c>
- <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/realtek/rtw88/rtw8822b.c>
- <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/realtek/rtw88/usb.c>

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
- The firmware transport/model starts only the separately packaged, pinned
  8822B image with truthful version/digest diagnostics; every negative case
  fails atomically.
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

Stop if the adapter descriptor differs from `p026`, efuse requires an unknown
RFE/calibration table, the pinned blob is incompatible, legal 2.4-GHz transmit
limits cannot be derived conservatively, firmware scan offload is mandatory,
or correct USB teardown requires changing p011/p015 ownership. Record the exact
boundary and return it to the owning Phase; do not broaden the USB match or
enable unrestricted channels as a workaround.

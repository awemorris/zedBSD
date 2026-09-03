# WS004 Phase 040: RTL8822BU passthrough regression reproduction

Last updated: 2026-09-03

WSID: `ws004`

Phase ID: `p040`

Combined ID: `ws004-p040`

Status: complete (`q068`)

Parent: [WS004 hardware](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Reproduce and classify the newly reported RTL8822BU direct-connect failure on
the exact `2357:012e` adapter through QEMU xHCI USB passthrough.  The observed
guest symptom is that `/sbin/wifi wlan1 list` succeeds while
`/sbin/wifi wlan1 connect SSID PASSPHRASE` returns `ENOENT` (`6`).

This Phase collects one trustworthy failing boundary only.  It does not repair
the driver, change WLAN policy, harden AX211, or treat a prior q059 success as
proof that the current candidate still works.

## Environment and safety boundary

- Authorized host: `awe@10.0.10.25`.
- Exact USB target: TP-Link `2357:012e`, currently unbound from a Linux network
  driver.
- Use `qemu-system-x86_64`, KVM/host CPU when available, q35, and a dedicated
  emulated xHCI controller for the passed-through adapter.
- Boot from a disposable copy of the current amd64 image.  Record source
  revision, image size/digest, firmware identity, QEMU command shape, and USB
  descriptor identity.
- Do not detach or pass through the host RTL8156 Ethernet device.  Do not
  assign AX211 to VFIO.  Verify that the SSH route remains usable before and
  after the run.
- Supply the controlled SSID/passphrase only at execution time.  They must not
  enter M/W/P/Q, tracked fixtures, retained logs, process listings, or command
  history.  Retained evidence redacts SSID, BSSID, MAC address, and key.

## Reproduction procedure

1. Build one current amd64 candidate with the RTL8822BU driver, `/sbin/wifi`,
   and the selected `rtl8822b-firmware` package.  Copy it to a private
   disposable directory on the authorized host and verify its digest there.
2. Confirm `2357:012e` remains unbound and pass only that device to QEMU.
3. Boot zedBSD, identify which `wlanN` belongs to `usb-rtl8822bu`, and bring
   that exact interface administratively up.
4. Start one fresh finite scan.  Wait for `scan state=complete`, then retain a
   redacted record of the target row's channel and security flags.
5. Invoke direct `/sbin/wifi` connect once.  Record elapsed stage, return
   number, immediate status, and nonsecret kernel diagnostics.
6. Classify the first boundary:
   - common snapshot selection: no new connection generation and immediate
     `ENOENT` because no exact, supported BSS was selectable;
   - RTL driver/firmware: a generation was admitted and later failed;
   - command/setup: the reported sequence cannot be reproduced because the
     interface, image, or scan precondition differs.
7. Stop QEMU, prove the host USB device and SSH route are in their original
   state, and remove credential-bearing disposable material.

One clean reproduction attempt is sufficient.  A second attempt is permitted
only to replace a demonstrably stale scan snapshot with a fresh completed
snapshot; it is not a repeatability campaign.

## Verification and evidence

- `git diff --check` for any planning/evidence updates.
- Current amd64 image build with `make -j16`; do not run aggregate
  `make check`.
- Host and guest evidence must identify exact source/image/USB identities,
  the RTL interface mapping, scan completion, redacted target channel/security,
  connect result/timing, immediate WLAN status, and first nonsecret kernel
  error boundary.
- Evidence belongs in `tests/` only after secret and network-identity review.
  Raw captures remain disposable and untracked.

## Completion conditions

Complete p040 only if the current candidate reproduces `ENOENT` and the
evidence distinguishes common BSS selection from an admitted RTL
driver/firmware connection failure.  If the exact bounded setup does not
reproduce it, mark the Phase `uncleared` with the immutable candidate and the
specific mismatch or next discriminator.

After p040, create a separate correction Phase from the observed first
boundary.  Do not preselect a security-policy, cache, radio, interrupt, or
firmware fix in this reproduction-only Phase.

## Result

Q068 reproduced the reported `ENOENT` on the exact `2357:012e` unit with the
current image, but localized it before the RTL connection state machine.  The
adapter attached as `wlan0`, one fresh scan completed as generation 1, and the
snapshot contained two redacted channel-1 BSS records.  The controlled 5-GHz
target was absent.  Direct connect returned `ENOENT` immediately; the
following status remained `state=idle`, `scan=complete`, with no authentication,
association, key, retry, or terminal error.

This agrees with the production boundary rather than an interrupt or firmware
failure: `station_select_bss_locked()` returns `ENOENT` when the exact SSID has
no supported snapshot record, while the RTL scan profile contains only
channels 1--11 and rejects channels above 11.  No connection generation was
admitted.  The invalid first keyboard-injection trial, which omitted the final
uppercase character, was discarded; the recorded result uses the exact
runtime credential.

The QEMU process was stopped, the adapter returned to its original unbound
interface state, and the independent host Ethernet route remained selected.
All raw credential-bearing captures and disposable remote images were removed.
The redacted evidence is retained in
[q068 RTL8822BU passthrough evidence](../tests/q068-rtl8822bu-passthrough-evidence.md).
The resulting correction is separately planned as
[`ws004-p041`](../phase041-rtl8822bu-5ghz-quality/phase.md); q068 made no source
change.

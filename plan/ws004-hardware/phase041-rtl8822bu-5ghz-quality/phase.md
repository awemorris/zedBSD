# WS004 Phase 041: RTL8822BU 5-GHz useful normal path

Last updated: 2026-09-03

WSID: `ws004`

Phase ID: `p041`

Combined ID: `ws004-p041`

Status: planned; Queue-ready after `q068`; not queued

Parent: [WS004 hardware](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Close the deterministic RTL8822BU capability gap isolated by q068: the exact
`2357:012e` adapter currently scans only 2.4-GHz channels 1--11, so a valid
5-GHz target never enters the common BSS snapshot and direct connect returns
`ENOENT` before the RTL connection state machine runs.

Establish one simple 5-GHz WPA2-Personal/CCMP useful normal path through scan,
selection, authentication, association, key installation, authorized L2,
DHCP, selected LAN-peer ping, and a bounded nonempty HTTP fetch.  RTL8822BU is
the prioritized WLAN target.  AX211 interrupt/lifecycle repair and broad WLAN
commonization remain separate work.

## Dependencies

- q068 / `ws004-p040`: exact-device reproduction and common-selection
  localization;
- `ws004-p028`: current 2.4-GHz radio and scan implementation;
- `ws004-p029`: existing WPA2/CCMP and L2 normal path;
- `ws004-p030`: existing automatic lifecycle and failure containment;
- `ws005-p004` and `ws005-p007`: primitive `/sbin/wifi` and higher-level
  network orchestration, used only after driver-level authorization works.

## Frozen scope

- Keep USB binding exact to the already accepted `2357:012e` descriptor.
- Replace the hard-coded channel-1--11 assumption with a checked table-driven
  2.4/5-GHz station scan contract.  Channel number, center frequency, band,
  RF/BB programming, calibration, TX-power selection, and rollback must remain
  internally consistent.
- Derive the permitted active/passive behavior from retained device/regulatory
  information.  Do not transmit on a channel whose permission cannot be
  established.  DFS operation and a general regulatory-policy UAPI are not
  prerequisites for the first useful non-DFS 5-GHz path.
- Preserve the working 2.4-GHz path.  A 5-GHz addition must not silently map a
  channel through the 2.4-GHz RF or TX-power tables.
- Keep credentials out of logs and planning.  Preserve the current common
  station, generation, cancellation, controlled-port, and secret-erasure
  contracts.
- Do not change public HAL APIs, invent a shared Intel/Realtek hardware layer,
  or repair AX211 in this Phase.

## Implementation sequence

1. Audit the imported RTL8822B RF/BB/channel and TX-power tables against the
   current `channel <= 11` guards and identify every band-dependent register,
   calibration, and rollback field before relaxing a guard.
2. Add a bounded channel descriptor table and focused production-source tests
   for valid non-DFS 5-GHz selection, invalid/forbidden channels, frequency
   mapping, power selection, rollback, cancellation, and preserved 2.4-GHz
   behavior.
3. Enable the minimum safe 5-GHz scan path, retaining finite per-channel and
   whole-scan deadlines and existing snapshot-generation semantics.
4. Use exact-device QEMU USB passthrough for one developmental normal-path
   checkpoint.  First require a redacted 5-GHz BSS row, then exercise direct
   WPA2/CCMP authorization and useful IP traffic.
5. Only after the normal path works, add the directly adjacent malformed,
   timeout, cancellation, detach, and radio-rollback cases exposed by the new
   band.  Do not expand this into unrelated exhaustive cleanup.

## Verification

- Focused ordinary and sanitizer tests for channel/frequency/power/calibration
  selection and rollback.
- Existing RTL scan, WPA2/CCMP, L2, lifecycle, USB/xHCI, and 2.4-GHz regressions.
- Configured amd64 and i386 builds with `make -j16`; never aggregate
  `make check`.
- One disposable amd64 OVMF/xHCI USB-root control.
- One exact `2357:012e` passthrough run on the authorized host.  Retain only
  redacted scan/security/state/traffic evidence and remove credential-bearing
  raw material.

## Completion conditions

- A fresh scan on the exact adapter publishes the controlled non-DFS 5-GHz BSS
  with correct channel/frequency/security metadata.
- Direct `/sbin/wifi` connection reaches authenticated, associated, keyed, and
  authorized state, followed by DHCP, selected LAN-peer ping, and a bounded
  nonempty HTTP fetch.
- The existing 2.4-GHz path and declared automatic lifecycle gates remain
  passing.
- Failure, cancel, down, and detach leave no stale generation, key, carrier,
  URB, DMA, or interface ownership.
- No AX211 quality or cross-driver-refactor completion is claimed.

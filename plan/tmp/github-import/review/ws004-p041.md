# WS004 Phase 041: RTL8822BU 5-GHz useful normal path

Last updated: 2026-09-04

WSID: `ws004`

Phase ID: `p041`

Combined ID: `ws004-p041`

Status: complete (`q071`); focused W52 gates and the shared user-accepted
physical WLAN result pass

Parent: [WS004 hardware](../ws.md)

Tests: [WS004 test index](../tests/README.md)

Primary test case: `HW-T41`

## Objective

Address the deterministic RTL8822BU capability gap isolated by q068: the exact
`2357:012e` adapter currently scans only 2.4-GHz channels 1--11, so a valid
5-GHz target never enters the common BSS snapshot and direct connect returns
`ENOENT` before the RTL connection state machine runs.

The user subsequently reported the same `ENOENT` for a snapshot-visible
2.4-GHz target. Q069's one exact attempt connected successfully, so that report
remains unlocalized and receives no speculative fix here. This Phase remains
the independently proven 5-GHz capability addition, not an assertion that the
separate 2.4-GHz observation was caused by the same gap.

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
- `ws005-p004`: the completed primitive `/sbin/wifi` path used for the first
  driver-level authorization checkpoint.  The later `ws005-p007` orchestration
  consumes this Phase; it is not a dependency and must not create a cycle.

## Frozen scope

- Keep USB binding exact to the already accepted `2357:012e` descriptor.
- Replace the hard-coded channel-1--11 assumption with a checked table-driven
  station profile containing the existing 2.4-GHz channels and only Japan
  non-DFS W52 channels 36, 40, 44, and 48 in the first implementation.
  Channel number, center frequency, band, RF/BB programming, calibration,
  TX-power selection, and rollback must remain internally consistent.
- Permit active operation on W52 only when the retained board/channel-plan
  facts establish the Japan policy.  Missing, malformed, or contradictory
  policy fails closed rather than guessing a regulatory domain.  DFS, W53,
  W56, channels 149 and above, passive-scan policy, and a general regulatory
  UAPI are outside this Phase.
- Preserve the working 2.4-GHz path.  A 5-GHz addition must not silently map a
  channel through the 2.4-GHz RF or TX-power tables.
- Extend the checked RTL8822B board record with the 5-GHz EFUSE power fields
  actually consumed by the W52 path.  Import the required legacy-OFDM
  by-rate/absolute-limit data into the existing BSD-3-Clause table boundary;
  do not reuse 2.4-GHz power values or mix their license/provenance records.
- Keep credentials out of logs and planning.  Preserve the current common
  station, generation, cancellation, controlled-port, and secret-erasure
  contracts.
- Do not change public HAL APIs, invent a shared Intel/Realtek hardware layer,
  or repair AX211 in this Phase.

## Implementation sequence

1. Extend the strict EFUSE/board parser with the RTL8822B 5-GHz BW40 base and
   signed legacy-OFDM difference fields, validating every consumed power value
   before the radio can expose W52.
2. Import the mechanically reduced 5-GHz legacy-OFDM by-rate and absolute
   power-limit data with its existing BSD-3-Clause provenance, then add focused
   tests for exact W52 power selection and rejection of unrepresented bands.
3. Make the 20-MHz RF/BB/CCA/RFE channel transaction band-aware, including
   CCK disablement, sub-band selection, RF18/RF-BE values, the W52 CCA tuple,
   RFE-specific front-end values, TXAGC calculation, and journal rollback.
4. Replace the USB driver's scan/connect channel-1--11 guards with membership
   in the 1--11 plus 36/40/44/48 profile.  Preserve finite per-channel and
   whole-scan deadlines and existing snapshot-generation semantics.
5. Use exact-device QEMU USB passthrough for one developmental normal-path
   checkpoint.  First require a redacted 5-GHz BSS row, then exercise direct
   WPA2/CCMP authorization and useful IP traffic.
6. Only after the normal path works, add the directly adjacent malformed,
   timeout, cancellation, detach, and radio-rollback cases exposed by the new
   band.  Do not expand this into unrelated exhaustive cleanup.

## Verification

- Focused ordinary and sanitizer tests for an exact channel-44 RF/BB/CCA/RFE/
  TXAGC transaction, invalid/DFS rejection, rollback/fail-close, and a 2.4-GHz
  round trip.
- USB-driver tests for the exact fifteen-channel profile, the final channel-48
  scan step, channel-44 connect, and preserved channel-1 behavior.
- Existing RTL scan, WPA2/CCMP, L2, lifecycle, USB/xHCI, and 2.4-GHz regressions.
- Configured amd64 and i386 builds with `make -j16`; never aggregate
  `make check`.
- One disposable amd64 OVMF/xHCI USB-root control.
- One exact `2357:012e` passthrough run on `10.0.10.25`, using a separate guest
  xHCI controller while leaving the host RTL8156 SSH route untouched.  Require
  the controlled channel-44/5220-MHz row, direct authorization, DHCP, selected
  LAN-peer ping, bounded nonempty HTTP fetch, disconnect, and down.  Retain
  only redacted evidence and remove credential-bearing raw material.

## Completion conditions

- A fresh scan on the exact adapter publishes the controlled W52 BSS with
  correct channel/frequency/security metadata, while an invalid regulatory
  record exposes no 5-GHz transmit channel.
- Direct `/sbin/wifi` connection reaches authenticated, associated, keyed, and
  authorized state, followed by DHCP, selected LAN-peer ping, and a bounded
  nonempty HTTP fetch.
- The existing 2.4-GHz path and declared automatic lifecycle gates remain
  passing.
- Failure, cancel, down, and detach leave no stale generation, key, carrier,
  URB, DMA, or interface ownership.
- No AX211 quality or cross-driver-refactor completion is claimed.

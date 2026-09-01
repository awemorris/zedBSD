# Queue: RTL8822BU WPA2-Personal/CCMP L2

Last updated: 2026-09-01

QID: `q058`

Queue status: in progress

Queue finished: **No**

Authorization: the user directed WLAN to remain the priority and authorized
automatic Queue continuation. Q057 completed the p028 automatic radio/scan
milestone. A read-only readiness audit found p029 sufficiently specified to
begin without a new human decision.

Timebox: none. Execute the one finite p029 automatic milestone. Do not request
a physical adapter, AP, credential, `/sbin/wifi`, DHCP, or WS005 acceptance in
this Queue.

Parent: [master plan](master.md)

Previous Queue: [q057](queue-q057.md)

## Purpose

Turn the q057 scan-only station into the smallest secure useful L2 station:
strict WPA2-Personal/RSN/PSK/CCMP authentication and association, the four-way
handshake, transactional hardware keys, controlled-port authorization, and
bidirectional Ethernet framing through the production common WLAN and
RTL8822BU paths. Complete all success, malformed, timeout, replay, rollback,
secret-lifetime, and synthetic data cases before any physical RF request.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p029` | [Phase](ws004-hardware/phase029-wpa2-ccmp-l2/phase.md) | in progress | Strict WPA2-Personal/CCMP handshake, transactional RTL keys, controlled port, and bidirectional synthetic Ethernet L2 pass every automatic gate |

## Accepted decisions

- Support only infrastructure open-system authentication followed by RSN v1,
  PSK, and CCMP-128. Reject or defer open/WEP/WPA1/TKIP/SAE/WPA3/802.1X,
  PMF-required, raw-hex PSKs, 5 GHz, HT/VHT optimization, and roaming.
- The common kernel WLAN layer owns BSS/security selection, authentication,
  association, passphrase derivation, nonces, EAPOL, keys, replay state,
  controlled-port state, and Ethernet/802.11 conversion. RTL code owns only
  BSSID/AID, SEC/CAM, TX/RX descriptor metadata, firmware reports, and device
  reset/quiesce.
- One direct-connect generation has a total 30-second monotonic deadline.
  Local waits and retries consume that budget and never restart it.
- A valid message 3 plus successful atomic pairwise/group key installation and
  successful message 4 transmission are required before carrier is raised.
  A driver shortcut cannot publish `AUTHORIZED`.
- If an AP advertises extra suites but has a complete CCMP+PSK choice, select
  that choice and construct a bounded association RSN element; do not reject
  it merely for advertising alternatives and do not copy its IE verbatim.

## Boundaries

- Preserve q057 power/radio/scan, generic USB, Mass Storage, CDC NCM/ECM, USB
  HID, wired network, net-device, and storage behavior.
- Keep new APIs internal to the existing kernel WLAN boundary unless a proven
  consumer requires a public change. Do not casually expand the frozen UAPI.
- Do not implement persistent profiles, `/sbin/wifi`, `net wifi`, `networkd`
  composition, DHCP, reconnect/rekey campaigns, or physical acceptance here.
  WS005 owns orchestration; p030 owns long-lived lifecycle hardening.
- Do not use `.internal/` or aggregate `make check`.

## Implementation checkpoints

1. Freeze the internal common-WLAN/radio callbacks needed for raw management,
   EAPOL, data, TX completion cookies, RX security metadata, BSSID/AID, and key
   generations. Remove/restrict the generic driver-authorized shortcut and
   correct strict-but-selective RSN choice.
2. Add bus-independent SHA-1, HMAC-SHA1, PBKDF2-HMAC-SHA1, AES-128, RFC 3394
   unwrap, constant-time comparison, and explicit secret erasure with official
   known-answer vectors and checked bounds.
3. Implement authentication, association, EAPOL messages 1--4, PMK/PTK/GTK,
   nonce/replay/retransmission rules, one atomic key transaction, finite state
   transitions, and a deterministic fake authenticator.
4. Add controlled-port gating, Ethernet/LLC-SNAP/802.11 conversion, CCMP
   metadata and packet-number rules, and valid/invalid bidirectional synthetic
   L2 traffic.
5. Implement RTL8822BU BSSID/AID, SEC/CAM install/delete, CCMP TX descriptor,
   encrypted RX status, and C2H TX report handling without moving protocol
   policy into the driver.
6. Exhaust handshake, crypto, downgrade, malformed, stale, timeout, entropy,
   CAM, USB, close/detach, and secret-lifetime faults; retain q057 and generic
   WLAN/storage regressions.

## Automatic gates

1. One focused production-linked runner passes crypto KATs, strict RSN/auth/
   association/EAPOL behavior, retransmission/replay defenses, atomic key
   rollback, controlled-port ordering, secret scrubbing, and L2 conversion in
   ordinary, ASan/UBSan, and compiler-analyzer modes.
2. RTL core/USB fixtures pass SEC/CAM/BSSID/AID, descriptor/RX-security/TX-
   report success and failure cases, endpoint ownership, stale generations,
   close/detach, and concurrent fake storage.
3. Existing p027/p028 common WLAN, table, radio, firmware, USB, net-device,
   Mass Storage, CDC, and storage-focused regressions remain passing.
4. Driver-enabled amd64 and i386 builds, ordinary `make -j16`, disposable IDE
   root and q35/xHCI USB-root exact-login controls, and `git diff --check` pass.

## Completion definition

Q058 completes when the production common WLAN and RTL8822BU paths can
automatically complete the declared WPA2-Personal/CCMP four-way handshake,
install and retire keys without replay/reinstall defects, authorize carrier at
the exact terminal boundary, and exchange bounded bidirectional synthetic
Ethernet L2 while every declared negative case fails closed and scrubs secret
state. Completion unblocks p030 and makes no physical-radio, DHCP, command, or
orchestration claim.

## Execution result

In progress. Q057 completed every radio/scan dependency. The readiness audit
identified no human decision or hardware input required for this automatic
milestone. Implementation begins with the internal contract and the two
existing shortcut/RSN-selection corrections before crypto and RTL work proceed
in parallel.

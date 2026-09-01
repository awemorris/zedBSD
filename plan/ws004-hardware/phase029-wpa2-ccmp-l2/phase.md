# WS004 Phase 029: WPA2-Personal/CCMP association and L2 data path

Last updated: 2026-09-01

Phase ID: `ws004-p029`

Status: in progress (`q058`); the `ws004-p028` automatic dependency is complete

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Complete one secure, useful station profile on the RTL8822BU target: strict
WPA2-Personal using the RSN protocol and CCMP-128, from BSS selection through
802.11 authentication/association, the EAPOL four-way handshake, hardware key
installation, controlled-port authorization, and bidirectional Ethernet L2
traffic. The common kernel WLAN layer remains the long-lived supplicant after
the short-lived control command exits.

DHCP, address persistence, credential-file policy, automatic network choice,
and `net wifi` orchestration belong to WS005. This Phase may use static IPv4
and ARP/ping only as a visible consumer of the completed L2 path.

## Dependencies

- `ws004-p027`: persistent common station state, generation/cancellation,
  UAPI, scan cache, controlled-port and driver callback contract.
- `ws004-p028` automatic milestone: exact RTL8822BU binding, pinned-firmware
  transport, synthetic 2.4-GHz scan, management TX/RX, USB descriptors, and
  key-CAM hooks, without requiring p028 physical feedback first.
- A cryptographically suitable `hal_entropy_fill()` result on amd64. Failure to
  obtain a fresh SNonce fails connection; deterministic or weak fallback is
  forbidden.
- A deterministic fake authenticator/peer configured for WPA2-Personal/AES on
  a 2.4-GHz non-DFS 20-MHz channel. The real controlled AP and peer are inputs
  only to the later shared WS005 p008 checkpoint, not a p029 gate.

Q058 executes this automatic milestone without a physical-radio dependency.

## Frozen security profile

Version 1 supports exactly:

- infrastructure station mode and open-system 802.11 authentication;
- RSN version 1;
- AKM suite `00-0f-ac:2` (PSK);
- pairwise and group cipher `00-0f-ac:4` (CCMP-128);
- an 8--63-octet passphrase converted to a 256-bit PMK with PBKDF2-HMAC-SHA1,
  SSID as salt, and 4096 iterations; and
- management/basic data rates and the p028 2.4-GHz/20-MHz radio profile.

Open networks, WEP, WPA1, TKIP, mixed WPA/WPA2 downgrade, raw 64-hex PSKs,
802.1X/EAP, SAE/WPA3, OWE, WPS, PMF-required BSSs, fast transition, roaming,
IBSS/AP/monitor modes, 5 GHz, HT/VHT optimization, QoS aggregation, and mesh
are rejected or deferred. An AP may advertise other pairwise suites only if a
complete CCMP+PSK choice remains; the association request advertises only the
selected subset and never copies an unreviewed RSN element verbatim.

## Long-lived ownership boundary

The common kernel WLAN layer owns:

- BSS/security selection, 802.11 authentication and association state,
  timeout/retry policy, and strict response parsing;
- passphrase-to-PMK derivation, fresh SNonce generation, PTK derivation and
  partition, EAPOL-Key parsing/construction/MIC/replay state, GTK extraction,
  and sensitive-buffer lifetime;
- key-generation identities, controlled-port admission, replay/reinstall
  prevention, carrier ordering, and the later rekey/reconnect state retained
  for `p030`; and
- 802.3 Ethernet to 802.11+LLC/SNAP encapsulation and the inverse validated RX
  conversion.

The RTL8822BU driver owns firmware/hardware operations only: channel/BSSID/AID
programming, management/EAPOL/data TX descriptors and completion, RX descriptor
and decryption status, CAM slot programming/deletion, CCMP offload selection,
and device reset/quiesce. It never derives a PMK from a passphrase, chooses a
network, advances the WPA state machine, or raises carrier by itself.

The common object retains PMK and the active connection generation only while
the interface remains up so `p030` can reauthenticate after transient loss. It
scrubs the original passphrase immediately after successful PMK derivation and
scrubs PMK/PTK/GTK/nonces/MIC state on explicit down, terminal failure, detach,
or shutdown. No secret appears in UAPI status, syslog, test evidence, crash
diagnostics, or USB traces.

## Ordered connection sequence

```text
scan snapshot and exact SSID
  -> strict RSN/CCMP/PSK BSS selection
  -> tune channel and send open-system Authentication
  -> validate Authentication response
  -> send Association Request with selected RSN and supported rates
  -> validate Association Response and AID
  -> receive EAPOL-Key message 1/4
  -> derive PTK; send message 2/4
  -> validate/decrypt message 3/4; install PTK and GTK once
  -> send message 4/4
  -> authorize controlled port and raise carrier
  -> Ethernet L2 traffic
```

Each transition has a finite deadline, a connection generation, a limited
retransmission count, and a precise terminal reason. The complete direct L2
connection generation has the frozen 30-second total monotonic deadline; every
authentication, association, EAPOL, key, driver, and retry wait uses the
smaller of its local bound and the remaining total, and no retry restarts that
budget. A scan completion or 802.11 association never raises carrier. Only a
valid message 3, successful atomic pairwise/group key install, and transmitted
message 4 authorize the controlled port.

## RSN, EAPOL, and key contract

1. Parse beacon/probe RSN elements with checked lengths and counts. Require one
   supported group cipher, at least one CCMP pairwise suite, at least one PSK
   AKM, no unsupported mandatory capability, and no trailing/truncated suite
   data. Prefer the p027 deterministic BSSID order only among equally supported
   candidates.
2. Build authentication and association frames from normalized fields. Check
   direction, BSSID/source/destination, sequence/type/subtype, algorithm,
   transaction, status code, AID bounds, rates, and response generation before
   advancing.
3. Accept only the WPA2 EAPOL-Key descriptor/version and exact Key Information
   bits appropriate to each message. Validate body/key-data lengths before
   reading them, require the expected authenticator address and ANonce, and
   compare MICs in constant time.
4. Treat the replay counter as monotonic within one handshake. A retransmitted
   message 1 or 3 may cause the matching response to be resent, but never
   regenerate SNonce, reinstall an already installed key, or reset a transmit/
   receive packet number. This is the explicit key-reinstallation defense.
5. Decrypt message-3 key data with the derived KEK and strict AES key-unwrapping
   bounds. Accept exactly the GTK KDE/key index required by the declared
   profile; reject duplicate, conflicting, unknown mandatory, malformed, or
   oversized key data before CAM modification.
6. Install PTK and GTK under one common-core transaction. If either driver
   operation fails, delete any staged slot, leave the port unauthorized and
   carrier down, send deauthentication when possible, and scrub transient
   material.

Pairwise and group rekey after an established connection are completed and
stress-tested in `p030`; the initial four-way handshake is complete here.

## Cryptographic substrate

Add small, bus-independent kernel primitives with known-answer tests rather
than embedding private crypto in the Realtek driver:

- SHA-1 and HMAC-SHA1;
- PBKDF2-HMAC-SHA1 with checked iteration/output arithmetic;
- the WPA pairwise-key expansion over ordered MAC addresses and nonces;
- AES-128 block encryption and RFC 3394 key unwrap; and
- constant-time equality plus explicit secret erasure.

Use fixed-width operations, avoid secret-dependent early comparison, and
reject overlapping/oversized buffers. The host fixture includes official
known-answer vectors from FIPS 197 and IETF RFC 2202, RFC 3394, and RFC 6070,
plus independently encoded WPA2 handshake frames. A test-only software CCMP
reference codec validates nonce/AAD/PN/key-ID/MIC expectations; the physical
RTL8822BU data path uses its reviewed CCMP hardware offload and key CAM.

Primary specifications and vectors:

- IEEE 802.11 standard landing page:
  <https://standards.ieee.org/ieee/802.11/7028/>
- NIST FIPS 197 AES:
  <https://csrc.nist.gov/pubs/fips/197/final>
- HMAC-SHA1 test cases, AES key wrap, and PBKDF2 vectors:
  <https://www.rfc-editor.org/rfc/rfc2202>,
  <https://www.rfc-editor.org/rfc/rfc3394>, and
  <https://www.rfc-editor.org/rfc/rfc6070>

## CCMP and Ethernet data path

Before authorization, only the common WPA state machine may transmit or
consume EAPOL (`0x888e`); all ordinary L2 TX is rejected with `ENETDOWN`, and
all ordinary RX is dropped. After authorization:

- TX accepts a bounded Ethernet frame, preserves source/destination/EtherType,
  adds RFC 1042 LLC/SNAP, builds a station-to-DS 802.11 data header for the
  selected BSSID, and submits the active pairwise key generation/CAM slot;
- the driver requests CCMP hardware encryption and reports completion without
  claiming success for a firmware reject or short USB completion;
- RX requires a valid from-DS data frame for the associated BSSID/station,
  successful CCMP decrypt/MIC status, expected key generation/key ID, and a
  non-replayed packet number before LLC/SNAP decapsulation and
  `net_device_receive()`;
- malformed headers, wrong direction/BSSID, null/QoS forms outside the profile,
  fragments, A-MSDU/A-MPDU forms not explicitly handled, unknown LLC/OUI,
  decrypt/MIC failure, stale key generation, duplicate PN, and oversize frames
  are dropped with separate counters; and
- disconnect lowers carrier and closes the controlled port before deleting
  keys or draining queued TX/RX, so no clear or stale-key packet escapes.

The initial MTU remains ordinary Ethernet 1500 only if every USB/802.11/CCMP
headroom calculation is checked. No jumbo or aggregation claim is made.

## Verification plan

`HW-T33` links production common-core, crypto, frame, and fake-driver code and
covers:

- every official crypto known-answer vector, multi-block/boundary/overflow
  cases, constant-time MIC comparison behavior, entropy failure, and erasure;
- strict RSN selection and rejection for every excluded security combination;
- successful authentication/association and every wrong address, transaction,
  status, AID, length, timeout, cancellation, and stale-generation response;
- exact 1/4--4/4 construction/validation, replay ordering, MIC failure, nonce
  mismatch, malformed/encrypted key data, missing/duplicate GTK, CAM failure,
  and controlled-port ordering;
- retransmitted message 1 and message 3 without SNonce regeneration, key
  reinstall, or PN reset;
- preauthorization filtering and valid bidirectional Ethernet/LLC/SNAP/CCMP
  reference frames, plus wrong BSSID/key/PN/MIC/direction/length/drop counters;
  and
- disconnect/close/detach at every handshake and TX/RX ownership point.

Run ordinary, ASan/UBSan, compiler-analyzer, configured amd64/i386, `make -j16`,
IDE boot, and xHCI USB-root regressions. Do not use `make check`.

## Physical-evidence handoff

p029 makes no independent zedBSD physical request. Successful and negative
RSN/authentication/association/EAPOL/key/CCMP cases, wrong passphrases,
unsupported security, malformed handshakes, and USB/key failures are exhausted
through the production common core and fake radio/USB fixtures first.

The first zedBSD association and data observation is the same single combined
checkpoint shared by p030 and
[`ws005-p008`](../../ws005-networking/phase008-archer-physical-acceptance/phase.md).
Its early L2 stages scan/select the controlled WPA2-Personal/CCMP SSID,
complete authentication/association/four-way/key installation, raise carrier
only after authorization, and exchange bounded bidirectional data before the
WS005 DHCP/lifecycle stages. p029 references the redacted management/EAPOL/key/
CCMP counters from that run and does not request a separate boot.

## Automatic milestone and later physical feedback

- The production common core plus fake-radio/USB driver completes every
  declared 802.11/EAPOL transition and rejects every downgrade/unsupported
  profile.
- Keys are derived, modeled through the RTL8822BU CAM/offload contract,
  installed, retired, and scrubbed with no replay-based reinstall or PN reset;
  carrier exactly follows controlled-port authorization.
- Synthetic bidirectional Ethernet/CCMP traffic passes the production L2 path,
  while preauthorization and invalid encrypted frames are blocked.
- `HW-T33` plus sanitizer/analyzer/build/storage regressions leave one candidate
  ready for p030 lifecycle work without claiming radio success.

Those conditions are the p029 automatic milestone and are sufficient for p030
to begin. The eventual actual RTL8822BU association/key/CCMP/L2 claim references
the single p030/WS005 p008 ledger after all automatic work, with no separate
p029 boot, secret disclosure, plaintext fallback, or DHCP substitution. This
feedback is not a prerequisite of p030 and therefore does not create a cycle.

## Reconsideration boundary

Return to planning if the firmware cannot expose reliable CCMP/key/PN status,
the chip requires a security mode outside the declared profile, correct WPA2
needs a general-purpose crypto API beyond this bounded substrate, entropy is
unavailable, or the AP requires PMF/SAE/802.1X. Do not disable MIC/replay checks,
fall back to TKIP/open, retain a passphrase in the driver, or raise carrier at
association merely to reach a data milestone.

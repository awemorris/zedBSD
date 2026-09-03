# WS004 Phase 042: RTL8822BU 2.4-GHz connect reproduction

Last updated: 2026-09-03

WSID: `ws004`

Phase ID: `p042`

Combined ID: `ws004-p042`

Status: complete (`q069`)

Parent: [WS004 hardware](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Correct q068's incomplete discriminator by attempting direct connection once
to the controlled 2.4-GHz SSID that is visibly present in the RTL8822BU scan
snapshot. Determine whether the user's `ENOENT` is reproduced even for an exact
channel-1 BSS, and if so whether common security selection or another
pre-admission condition rejects it.

This is a one-attempt reproduction Phase. It does not implement 5-GHz support,
change security policy, repair the driver, harden AX211, or claim useful WLAN
connectivity.

## Environment and fixed boundary

- Reuse the immutable q068 candidate at source revision
  `978a1a215347f3f7fb82f0cf55d114265fa8867c`, image SHA-256
  `a4eb674cea41365876255c7bceed853528b67a7013ac460eb0d354fe7bb92eaa`.
- Authorized host and exact USB device remain `awe@10.0.10.25` and
  `2357:012e`.
- Use q35/KVM/OVMF, an xHCI USB-root controller, and a separate xHCI controller
  containing only the RTL8822BU.
- Do not pass through RTL8156 or assign AX211. Preserve the independent SSH
  route.
- Use the explicitly requested 2.4-GHz SSID and its previously supplied key
  only at runtime. Retained plans and evidence redact SSID, BSSID, station MAC,
  and key; delete raw credential-bearing captures.
- Execute exactly one valid direct-connect attempt after one completed fresh
  scan. Do not retry it.

## Procedure

1. Copy the immutable image to a private disposable host directory and verify
   its digest.
2. Pass through only `2357:012e`, boot, identify its `wlanN`, bring it up, and
   complete one fresh finite scan.
3. Verify that an exact SSID row exists, and retain only its redacted channel,
   frequency, security flags, and scan generation.
4. Invoke `/sbin/wifi INTERFACE connect SSID PASSPHRASE` exactly once and
   record return, elapsed boundary, immediate public status, and nonsecret
   kernel diagnostics.
5. Classify the first boundary against production common selection and RTL
   admission code. Stop QEMU, verify host restoration, and delete raw material.

## Completion conditions

Complete p042 when the one exact 2.4-GHz attempt is recorded and classified,
whether it reproduces `ENOENT`, advances into the RTL state machine, or
succeeds. Update p041 from that evidence before placing any correction in a
later Queue. No source change belongs to q069.

## Result

The single authorized attempt succeeded. The exact RTL8822BU attached as
`wlan0`; one fresh scan completed as generation 1 with two redacted channel-1
BSS records. The supported WPA2/CCMP/PSK transition-mode record was selected,
the connection completed as generation 2, and public status reported
authenticated, associated, keyed, and authorized with zero retries and zero
terminal error.

Therefore q069 did not reproduce the user's 2.4-GHz `ENOENT`. It proves only
one useful normal-path connection on the immutable q068 image; it does not
invalidate the user's observation or justify a speculative repair. The one-run
record is [q069 evidence](../tests/q069-rtl8822bu-24ghz-evidence.md). QEMU was
stopped, host USB and network ownership were restored, and all raw
credential-bearing material was deleted.

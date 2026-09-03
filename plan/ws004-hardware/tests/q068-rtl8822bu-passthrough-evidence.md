# Q068 RTL8822BU passthrough reproduction evidence

Date: 2026-09-03

Phase: [`ws004-p040`](../phase040-rtl8822bu-passthrough-reproduction/phase.md)

Result: PASS -- the reported `ENOENT` was reproduced and localized to common
BSS selection before any RTL connection attempt was admitted.

## Immutable candidate

- source revision: `978a1a215347f3f7fb82f0cf55d114265fa8867c`;
- configured build: `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk
  BUILD=build/q068-amd64 disk-image`;
- image: `253755392` bytes, SHA-256
  `a4eb674cea41365876255c7bceed853528b67a7013ac460eb0d354fe7bb92eaa`;
- selected package: `rtl8822b-firmware` version `30.20.0`, firmware SHA-256
  `a72da690597bfa99d8eb6fc2ab090d18d8ad92ac2befd35db1c9e3662d8d8418`;
- QEMU: `10.0.11 (Debian 1:10.0.11+ds-0+deb13u1)` with q35, KVM host CPU,
  four vCPUs, 1 GiB, OVMF, a dedicated xHCI USB-boot controller, and a
  separate xHCI controller containing only USB `2357:012e`;
- OVMF code SHA-256:
  `624e06de18b4fa535e90db7160d00d3d07d206422b89999bf1e27d920264e4e0`.

The USB descriptor reported high-speed USB 2.10, one vendor-specific
interface, bulk IN endpoint 4, bulk OUT endpoints 5, 6, and 8, and interrupt IN
endpoint 7.  The host RTL8156 and AX211 were not assigned to the guest.

## Redacted observation

The adapter attached through the production driver as `wlan0`.  After
administrative up, the bounded scan returned:

```text
search state=scanning generation=1 error=0
scan state=complete generation=1 results=2 truncated=no error=0
0 ssid=<redacted> bssid=<redacted> channel=1 frequency=2412 rssi=-76
  security=privacy+WPA2+CCMP+PSK+SAE+PMF-capable flags=0x000001b5
1 ssid=<redacted> bssid=<redacted> channel=1 frequency=2412 rssi=-76
  security=privacy+WPA2+CCMP+SAE+PMF-capable+PMF-required flags=0x00000395
```

The requested controlled 5-GHz target was not in that completed snapshot.
The exact direct command then returned immediately:

```text
wifi: "wlan0": connect: No such file or directory (6)
state=idle scan=complete administrative=up authenticated=no associated=no
  key=no authorized=no retries=0 error=0
```

No nonsecret kernel connection failure followed.  The unchanged idle state,
zero retries/error, and synchronous return establish that the driver did not
admit authentication or association.

## Classification

The production common core requires an exact SSID with an accepted security
record and returns `ENOENT` otherwise.  The production RTL8822BU implementation
currently declares exactly channels 1--11 and rejects a scan channel above 11.
Therefore this observation is a deterministic capability gap: the 5-GHz BSS
cannot enter the common snapshot.  It is not evidence of the AX211 interrupt
stop, an RTL interrupt stop, or a post-admission WPA failure.

The first input-injection trial omitted an uppercase character and was rejected
as command/setup evidence.  The result above is the subsequent exact runtime
input on the same fresh generation-1 snapshot; this is one valid reproduction,
not a repeatability campaign.

## Host restoration and disposal

After QEMU exit, USB `2357:012e` was again owned only by the generic Linux USB
core with no interface network driver, and the independent USB-Ethernet route
to the selected LAN probe was unchanged.  The remote QEMU directory, guest
copy, OVMF variable copy, and raw debug log were deleted because the log had
echoed the runtime credential.  No SSID, BSSID, station MAC, or passphrase is
retained here.

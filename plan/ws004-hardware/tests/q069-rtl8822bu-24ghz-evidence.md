# Q069 RTL8822BU 2.4-GHz one-attempt evidence

Date: 2026-09-03

Phase: [`ws004-p042`](../phase042-rtl8822bu-24ghz-connect-reproduction/phase.md)

Result: PASS -- the one authorized snapshot-visible 2.4-GHz attempt connected;
the reported `ENOENT` did not reproduce.

## Candidate and topology

- source revision: `978a1a215347f3f7fb82f0cf55d114265fa8867c`;
- image: `253755392` bytes, SHA-256
  `a4eb674cea41365876255c7bceed853528b67a7013ac460eb0d354fe7bb92eaa`;
- exact USB target: `2357:012e`, attached alone to a dedicated guest xHCI;
- q35/KVM/OVMF, four vCPUs, 1 GiB, separate xHCI USB-root controller;
- RTL8156 and AX211 were not assigned to the guest.

## Redacted result

```text
search state=scanning generation=1 error=0
scan state=complete generation=1 results=2 truncated=no error=0
selected BSS: channel=1 frequency=2412 rssi=-77
security=privacy+WPA2+CCMP+PSK+SAE+PMF-capable flags=0x000001b5
connected state=connected generation=2 authorized=yes
state=connected scan=complete administrative=up authenticated=yes
associated=yes key=yes authorized=yes retries=0 error=0
channel=1 frequency=2412 rssi=-77
```

The exact requested 2.4-GHz SSID appeared in generation 1 and the direct
connection was invoked exactly once. No retry was made. This result establishes
one working normal path but does not explain the user's separately observed
`ENOENT`; a later investigation requires a differing immutable artifact or
captured scan/status boundary rather than speculation.

After QEMU exit, the adapter interface again had no Linux network driver, the
independent Ethernet route was unchanged, and the disposable image, OVMF
variables, and credential-echoing raw log were deleted. No SSID, BSSID, station
MAC, or passphrase is retained here.

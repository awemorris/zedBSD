# Q080 Archer T3U Plus implementation evidence

Last updated: 2026-09-06

Phase: [ws004-p046](../phase046/phase.md)

Status: finished; p046 uncleared, continuing in q081

## Scope and identity

The user authorized implementation and exact-device 2.4-GHz plus 5-GHz useful
communication after [p045 intake](archer-t3u-plus-intake.md). The target remains
`2357:0138`, currently SuperSpeed `0300/0300`, bulk MPS 1024, burst 3.
Supplied credentials remain only in the user-authorized private file and
transient guest memory/store. They are not evidence artifacts.

## Implementation and automatic evidence

- Exact Nano HS, Plus HS and Plus SS profiles; strict configuration/interface/
  companion validation, private bulk packet profile and board diagnostics.
- Speed-selected RXDMA (`1e` HS, `0e` SS), unchanged v1 aggregation (`2005`),
  checked cut-D USB PHY initialization, retained short-packet padding.
- Source review additionally localized fixed 1-Mbit/s CCK management frames on
  W52 and CCK-only probe rate IEs. Both corrections and their ordinary,
  sanitizer and analyzer regression gates pass.
- Initial USB-driver, radio-core and security gates pass ordinary, ASan/UBSan
  and GCC analyzer builds; the core runner also passed with the pinned binary
  supplied through `RTL8822B_FIRMWARE_TEST_BLOB`, exercising its loader gates.
- Serialized amd64, PC/AT and PC-98 `make -j16` image builds pass after the
  management-rate, probe-IE and measured channel-policy corrections. Cell 3
  rebuilt amd64 with additional diagnostics; final transport changes require
  another serialized build before image freeze.

## Runtime procedure

[wlan-probe-guest.c](wlan-probe-guest.c) disables terminal echo while reading
runtime credentials and uses the production fixed-store writer. It then
invokes normal `net wifi` commands and reports real child exit status.
[run-rtl8822bu-passthrough.py](run-rtl8822bu-passthrough.py) takes credentials
over stdin, keeps raw console data in memory, retains only redacted output,
serves a bounded local HTTP payload and verifies both bands through the
production networkd key-fd path. Its command arguments contain no passphrase.

The runner uses QEMU/KVM q35 with USB-root and separate WLAN xHCI controllers,
no emulated NIC, and only the exact USB adapter. It records an input hash,
checks device presence/unbound state and unchanged management routes on exit,
and removes the disposable image containing the guest credential store.

## Cell 1: attach and asynchronous-scan harness correction

The first launch used runtime SHA-256
`50fa65723ee4fd296963adce40246a7e7523db76da4fae0137548e9dc7286c05`.
It booted, provisioned the private store, bound the exact Plus and completed
administrative up. Attach reported cut `3` (D), RFE `3`, USB SS, bulk `1024`,
country `ffff`, channel plan `a6`.

The harness requested connect while `net wifi list` still reported scan state
`1`, generation `0`, zero available results. The resulting `ENOENT` is not an
AP authentication failure. The corrected harness waits for completed scan
state `2` before selecting a stored profile. No useful-band acceptance is
claimed from cell 1. Target presence/unbound state, original management routes
and input hash were restored/verified; the secret-bearing guest copy was deleted.

The measured board's missing country and different channel plan require
primary-source analysis before changing W52 admission. Independent review also
found that common WLAN probe IEs always advertised CCK rates; the bounded
band-aware IE correction belongs to this requested 5-GHz path.

## Measured channel-policy correction

The source now admits the exact erased-country `ffff`, hardware-enforced `a6`
factory profile for calibrated W52 channels only. Bit 7 indicates that the
hardware plan is authoritative; lower bits `26` identify WORLD_ETSI1. Its
5-GHz domain includes channels 36/40/44/48 without DFS. The existing table
already uses the minimum FCC/ETSI/MKK power limit, so no power table or country
override is introduced. Unforced `26`, unknown plans, contradictory country
bytes, DFS and invalid calibration remain rejected; the prior JP profiles work.

Primary sources:

- [Vendor plan flag decoder](https://github.com/RinCat/RTL88x2BU-Linux-Driver/blob/0026128fa37398416bc14a3220e85596b84cf6c7/hal/hal_com.c#L362)
- [Vendor channel-plan and channel-set definitions](https://github.com/RinCat/RTL88x2BU-Linux-Driver/blob/0026128fa37398416bc14a3220e85596b84cf6c7/core/rtw_chplan.c#L129)
- [Linux WORLD_ETSI1 name](https://github.com/torvalds/linux/blob/v6.19/drivers/net/wireless/realtek/rtw88/regd.h#L12)

These sources establish hardware semantics; no vendor GPL source or table was
copied into the implementation. Existing imported BSD table provenance is
unchanged. Core normal/sanitizer/analyzer gates pass the exact parsed cut-D,
RFE3, SS board through W52 startup, channel selection and management TX.

## Cell 2: interface quarantine during scan

Runtime SHA-256:
`2e817154ef70d32fc37aafe015c1480535043b6b65254968b191fc0d29906be0`.
The corrected asynchronous wait observed two running-scan snapshots, then
`net wifi list` returned `ENETDOWN` before any connection attempt. This is a
driver recovery/quarantine boundary, not WPA2 rejection. The existing logs did
not retain the originating error; cell 3 adds nonsecret control-transfer,
channel-failure and recovery diagnostics. No USB speed or RF policy workaround
was inferred from the terminal errno. Host presence/unbound state, route and
source hash checks all passed, and the credential-bearing guest was deleted.

## Cell 3: missing firmware TX reports

Runtime SHA-256:
`4dcdeb2952abfda748a410db8c273a85d1d399c9f00ca4bfe4b65c4adcf22f74`.
The added diagnostics report recovery with `ETIMEDOUT` (42), three TX
tombstones, zero RX-error streak and zero control-error streak. No failed
channel-switch diagnostic occurred; recovery completed with error zero.
The failure precedes authentication and the W52 part of the scan. Layer-2
interface counters alone cannot establish whether USB RX/C2H traffic arrived.
All host restoration and image hash checks pass, and the guest copy was deleted.

Independent primary-source comparison found an omitted USB protocol step:
after an ON-section register access (`0000..00ff` or `1000..10ff`), issue a
one-byte vendor write to `04e0`, carrying the first byte of the completed
transaction. Both [Linux rtw88](https://github.com/torvalds/linux/blob/v6.19/drivers/net/wireless/realtek/rtw88/usb.c)
and the [pinned vendor implementation](https://github.com/RinCat/RTL88x2BU-Linux-Driver/blob/0026128fa37398416bc14a3220e85596b84cf6c7/os_dep/linux/usb_ops_linux.c#L160)
perform this operation. The missing step is a confirmed compatibility delta;
its causal relationship to the observed timeout remains unproven. Cell 4
will include the checked completion operation and bounded RX/C2H diagnostics.
Other reviewed SuperSpeed RXDMA, aggregation, FIFO, PHY and packet-padding
settings match the upstream RTL8822B path; no speculative speed change is made.

## Cell 4: receive path works, TX notifications remain absent

Runtime SHA-256:
`fc2bde23ee7921a6df3d5ce1a620589febfe962b26a8a02f959ad8efabfaf940`.
The checked ON-section completion operation passed independent review and the
ordinary/sanitizer/analyzer USB driver gates. Final configured amd64, PC/AT
and PC-98 builds pass. The frozen production SHA-256 is
`fc92f1eb54bed8125a9a8f451a5cf963fefeccc81d9cac4d1371c04ec546df70`;
the helper root filesystem remains
`83efe8200af3f176f1fc15baa509e3522cea289c085d395b720abb22473a7187`.

Hardware still reached recovery with timeout 42 and three TX tombstones.
The new counters measured 78 USB completions, 78 parsed wireless frames,
zero C2H notifications and last transfer length 548. This proves actual USB
and RF receive activity, and narrows follow-up to TX/report generation or
delivery. The companion operation alone did not fix the observed failure.
Recovery succeeded; exact target presence/unbound state, management routes
and source hash were verified, and the credential-bearing guest was deleted.

Q080's four-launch budget is exhausted. It closes with p046 uncleared because
neither band's useful communication is established. [Q081](q081-results.md)
continues under the existing user execution request, using the new RX-positive,
C2H-zero evidence to inspect TX queue/descriptor/firmware-report configuration.

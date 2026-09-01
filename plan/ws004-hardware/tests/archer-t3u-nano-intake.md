# HW-T32 Archer T3U Nano intake evidence

Last updated: 2026-09-01

Phase: `ws004-p026`

Status: complete (`q055`); q040 read-only intake plus the exact-unit label and
firmware-package decisions are closed

## Exact development unit descriptor

The adapter was already connected, unbound, to the user-authorized development
host `awe@10.0.10.25`. The one read-only inventory used:

```text
Host OS: Debian GNU/Linux 13.6 (trixie)
Kernel: Linux 6.19.13+deb13-amd64 x86_64
Command: sudo lsusb -v -d 2357:012e
Topology: xHCI bus 003 port 3, no host driver, High Speed (480 Mbps)
```

Serial text is deliberately redacted. The retained descriptor is:

```text
Device Descriptor:
  bLength                18
  bDescriptorType         1
  bcdUSB               2.10
  bDeviceClass            0
  bDeviceSubClass         0
  bDeviceProtocol         0
  bMaxPacketSize0        64
  idVendor           0x2357
  idProduct          0x012e
  bcdDevice            2.10
  iManufacturer           1 Realtek
  iProduct                2 802.11ac NIC
  iSerial                 3 [redacted]
  bNumConfigurations      1

Configuration 1:
  wTotalLength         0x0035
  bNumInterfaces            1
  bmAttributes           0x80 (bus powered)
  MaxPower              500 mA

Interface 0 alternate 0:
  bNumEndpoints             5
  bInterfaceClass        0xff
  bInterfaceSubClass     0xff
  bInterfaceProtocol     0xff
  endpoint 0x84: bulk IN,       max packet 512, interval 0
  endpoint 0x05: bulk OUT,      max packet 512, interval 0
  endpoint 0x06: bulk OUT,      max packet 512, interval 0
  endpoint 0x87: interrupt IN,  max packet  64, interval 3
  endpoint 0x08: bulk OUT,      max packet 512, interval 0

BOS:
  USB 2.0 extension: LPM supported
  reported supported speeds: Full Speed and High Speed
  lowest fully functional speed: Full Speed
  U1 exit latency: 10 microseconds
  U2 exit latency: 1023 microseconds

Device status: bus powered
```

The positive match is the exact combination of `2357:012e`, device revision
`2.10`, interface `ff/ff/ff`, and the five-endpoint tuple above. This retained
descriptor is the binding authority for the exact purchased Japan-market unit;
it is not a vendor-wide, product-string, marketing-revision, or generic
vendor-class match.

## Independent 8822B mappings

The official TP-Link archive was inspected without executing its installer:

| Property | Value |
| --- | --- |
| URL | `https://static.tp-link.com/upload/driver/2025/202512/20251231/Archer%20T3U%20Nano.zip` |
| Size | 15,360,959 bytes |
| SHA-256 | `d6d960744ebd010b8f25e3acd326bd1c142e20823dda016a4c2a526d3b85008b` |
| INF | `Archer T3U Nano/plugins/Driver Files/Driver/Windows_10_64bit/netrtwlanu.inf` |
| INF size | 352,944 bytes |
| INF SHA-256 | `00081504049ab682817180e45a2d973b7cfbc74bd7badec72f19e77c5155903f` |

Its `For 8822B TPLINK` section contains:

```text
%TPLINK_012E.DeviceDesc% = U2_RTL8812bu.ndi, USB\VID_2357&PID_012E
```

and the same INF identifies the family as `Realtek 8822BU Wireless LAN
802.11ac USB NIC`. The `RTL8812bu` install-section spelling is the vendor
driver's shared internal section name; it does not override the surrounding
8822B classification.

Linux mainline revision
`a23cbb05744b22719efdc34f9e329a120e81e617` was inspected at
`drivers/net/wireless/realtek/rtw88/rtw8822bu.c` (file SHA-256
`f887ae7ff81dc2d0e76268236bce291be8e48850d841fa4a70d8ae75de082569`).
Its exact `USB_DEVICE_AND_INTERFACE_INFO(0x2357, 0x012e, 0xff, 0xff,
0xff)` entry selects `rtw8822b_hw_spec`.

These two independent mappings agree with the FCC V1.0 internal-photo record.
They reject the earlier RTL8828BU guess. The FCC revision is documentary family
evidence only: the purchased unit has no printed revision and is not asserted
to be V1.0.

## Pinned firmware and license boundary

The pinned upstream is official `linux-firmware` commit
`458e40fdbb4dad5134ec230a42df21aea1b5baf8`:

| Property | Value |
| --- | --- |
| Blob path | `rtw88/rtw8822b_fw.bin` |
| Installed path | `/lib/firmware/rtw88/rtw8822b_fw.bin` |
| Size | 161,240 bytes |
| SHA-256 | `a72da690597bfa99d8eb6fc2ab090d18d8ad92ac2befd35db1c9e3662d8d8418` |
| Header | signature `0x8822`, category `0`, function `0` |
| Reported version | `30.20.0` (version `30`, subversion `20`, subindex `0`) |
| WHENCE SHA-256 | `34f954c7d068ec4fd5fcc216471912dd3cf40ff60a7ffa8d06ff6f9b5999551f` |
| License path | `LICENSES/LICENCE.rtlwifi_firmware.txt` |
| License size | 2,115 bytes |
| License SHA-256 | `a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e` |

The approved acquisition mirror is
`https://github.com/endlessm/linux-firmware.git` at immutable revision
`2f56219d20e4becccd718963fc3bcc671c543ce5`. At that revision,
`rtw88/rtw8822b_fw.bin` is byte-identical to the pinned official-upstream blob,
and the mirror's root `LICENCE.rtlwifi_firmware.txt` is byte-identical to the
pinned license. The mirror is transport only: official provenance remains the
`linux-firmware` commit and WHENCE record above, and the frozen size and
SHA-256 values are authoritative.

The pinned WHENCE entry says that the rtw88 firmware was supplied by a Realtek
engineer and directs redistribution to the exact license above. That license
permits unmodified binary redistribution with its copyright/disclaimer,
forbids endorsement and reverse engineering/decompilation/disassembly, and
contains the stated limited patent license. It is not represented as zlib-
licensed base-system source.

The approved optional package identity and source root are `wifi-firmware` and
`userland/packages/wifi-firmware/`. P028 will add the recipe and manifest with
the frozen mirror revision, hashes, provenance, and update rule, but never the
Realtek blob. Once implemented, only explicit selection/build of that package
downloads the unmodified blob and license into ignored build storage, verifies
both frozen SHA-256 values, and stages the blob for separate installation at
the fixed path. An ordinary base build performs no firmware download, an
ordinary image contains no blob, and the kernel never performs a runtime
network fetch.

## Later HW-T31 negative inputs

`ws004-p028` generates these only from the separately supplied pinned blob;
none is committed to the base tree:

| Case | Construction | Required classification |
| --- | --- | --- |
| absent | no file at the fixed path | `ENOENT`, interface remains down |
| short | retain only bytes 0--30 | truncated header/file, not digest mismatch |
| oversized | append one zero byte (161,241 bytes) | wrong size before upload |
| wrong digest | flip bit 0 at offset 64, preserving the header | substituted payload/digest mismatch |
| unsupported header | replace signature bytes `22 88` with `00 00` | unsupported header before upload |

Every case leaves carrier down, publishes no working-radio claim, and performs
no partial upload. A newer upstream blob is the wrong-digest case until a
separate package review advances every pinned field.

## q055 final label observation and authority decision

The purchased product is labelled only `Archer T3U Nano`, its region is Japan,
and no separate `Ver:` or hardware-revision marking is present. The serial
remains omitted. Absence of a printed revision is the observed value; it is not
filled from `bcdDevice=2.10` and does not turn the FCC V1.0 record into a claim
about this unit.

The user accepts the complete q040 descriptor as the binding authority for this
exact unit. The initial driver therefore matches only the retained
`2357:012e`, `bcdDevice=2.10`, `ff/ff/ff`, five-endpoint profile. A future unit
with a different descriptor returns to identity planning rather than widening
the match. This decision closes p026 and releases p027; it supplies no zedBSD
radio result.

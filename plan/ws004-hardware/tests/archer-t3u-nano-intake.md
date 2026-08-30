# HW-T32 Archer T3U Nano intake evidence

Last updated: 2026-08-31

Phase: `ws004-p026`

Status: automatic/read-only intake complete; printed product label fields are
the only missing evidence

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

The future positive match is the exact combination of `2357:012e`, device
revision `2.10`, interface `ff/ff/ff`, and the five-endpoint tuple above. It is
not a vendor-wide, product-string, or generic vendor-class match. A different
printed hardware revision remains a stop condition even if it reuses these
descriptor fields.

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
They reject the earlier RTL8828BU guess, but do not identify an unobserved
printed revision of the purchased unit.

## Pinned firmware and license boundary

The pinned upstream is official `linux-firmware` revision
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

The pinned WHENCE entry says that the rtw88 firmware was supplied by a Realtek
engineer and directs redistribution to the exact license above. That license
permits unmodified binary redistribution with its copyright/disclaimer,
forbids endorsement and reverse engineering/decompilation/disassembly, and
contains the stated limited patent license. It is not represented as zlib-
licensed base-system source.

The optional package identity is `firmware-rtw8822b`. It must contain the
unmodified blob, the exact pinned license text, this upstream revision/path,
size and digest, and an explicit update record. The zedBSD base contains only
the native source and fixed request path. It contains neither the blob nor a
build-time/runtime download, and a normal image cannot silently obtain it.

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

## Remaining physical evidence

The descriptor cannot establish what is printed on the enclosure or package.
To clear p026, one observation must still supply, with the serial omitted:

- printed model;
- printed region; and
- printed `Ver:`/hardware revision.

Until then, p026 is `uncleared`, p027/p028 do not bind this purchased adapter,
and no inference from `bcdDevice=2.10` fills the missing label field.

# HW-T43 Archer T3U Plus feasibility evidence

Last updated: 2026-09-06

Phase: [ws004-p045](../phase045-archer-t3u-plus-feasibility/phase.md), complete

Implementation follow-up: [ws004-p046](../phase046-archer-t3u-plus-driver/phase.md), planned

## Exact target and host

The user identifies the attached product as **TP-Link Archer T3U Plus** and
authorizes use of `ssh awe@10.0.10.25`. No printed hardware revision or market
label was independently inspected. Upstream's `0138` V1 name is documentary
evidence; the measured tuples below are the exact device authority.

Host: Debian, `6.19.13+deb13-amd64`, build
`6.19.13-1~bpo13+1 (2026-04-26)`, x86_64. Passwordless sudo is available.
Driver: Debian's installed `rtw88_8822bu` module, with a `2357:0138` USB alias.
The management route uses the separate RTL8156/r8152 wired interface; the
Intel AX211 remains a separate device. Neither was detached or reconfigured.

## USB identity before and after Linux mode switching

Commands: `lsusb -t`, `sudo lsusb -v -d 2357:0138`, and a byte read of the
selected sysfs `descriptors` file. Serial/MAC/SSID/BSSID are omitted here.

| Field | Initial profile | After Linux probe |
| --- | --- | --- |
| VID:PID | `2357:0138` | `2357:0138` |
| Sysfs USB location | `3-3`, interface `3-3:1.0` | `4-4`, interface `4-4:1.0` |
| Actual speed | High Speed, 480 Mbit/s | SuperSpeed, 5000 Mbit/s |
| bcdUSB / bcdDevice | `0210` / `0210` | `0300` / `0300` |
| Device class/subclass/protocol | `00/00/00` | `00/00/00` |
| EP0 descriptor byte | `64` (64 bytes) | `9` (2^9 = 512 bytes) |
| Configurations / selected configuration | `1` / `1` | `1` / `1` |
| Configuration length / attributes / power | `53` / `80` / 500 mA | `83` / `80` / 504 mA |
| Interface / alternate / count / class | `0` / `0` / one interface, one alternate / `ff/ff/ff` | same |
| Endpoint count | `5` | `5` |
| Bulk IN / OUT addresses | IN `84`; OUT `05`, `06`, `08` | same |
| Bulk attributes / MPS / interval | `02` / `512` / `0` | `02` / `1024` / `0` |
| Bulk SS companions | absent | bMaxBurst `3`, attributes `0`, wBytesPerInterval `0` |
| Interrupt endpoint | IN `87`, attributes `03`, MPS `64`, interval `3` | same |
| Interrupt SS companion | absent | bMaxBurst `0`, attributes `0`, wBytesPerInterval `64` |
| Manufacturer / product | `Realtek` / `802.11ac NIC` | same |

The initial BOS capability advertised Full/High Speed (`wSpeedsSupported=6`);
the post-switch BOS advertises Full/High/SuperSpeed (`0x000e`). Linux's
`rtw88_usb.switch_usb_mode` parameter was `Y` and was not changed.

Retained post-switch descriptor bytes, one descriptor per line (string contents
are not part of these bytes):

```text
120100030000000957233801000301020301
09025300010100803f
0904000005ffffff02
07058402000400
063003000000
07050502000400
063003000000
07050602000400
063003000000
07058703400003
063000004000
07050802000400
063003000000
```

## Firmware finding and fresh acquisition

Before the diagnostic, no `rtw88/rtw8822b_fw.bin` file was installed and
`dpkg-query -W firmware-realtek` reported no matching installed package.
Kernel timestamps below are monotonic seconds:

```text
[186026.025589] rtw88_8822bu 3-3:1.0: firmware: failed to load rtw88/rtw8822b_fw.bin (-2)
[186026.032036] rtw88_8822bu 3-3:1.0: probe with driver rtw88_8822bu failed with error -22
```

The first error is ENOENT; the later probe EINVAL does not establish a binary
compatibility failure. The old `012e` adapter had the same missing-file error
in the retained host kernel log. This observation does not compare all Debian
firmware-package versions or rule out unrelated reports about other systems.

The unchanged production package recipe completed a fresh acquisition:

```sh
make -j16 -C userland/firmware/rtl8822b download \
  RTL8822B_FIRMWARE_CACHE_ROOT=/home/awe/zedBSD/plan/ws004-hardware/temp/p045-firmware
```

- GitHub repository: `endlessm/linux-firmware`
- Immutable revision: `2f56219d20e4becccd718963fc3bcc671c543ce5`
- Blob: `rtw88/rtw8822b_fw.bin`, 161240 bytes, version `30.20.0`
- SHA-256: `a72da690597bfa99d8eb6fc2ab090d18d8ad92ac2befd35db1c9e3662d8d8418`
- The package also verified its pinned license and mirror WHENCE. No binary
  was added to tracked source, and the production cache/images were not rebuilt.

Primary-source comparison found byte-identical NIC firmware in the official
linux-firmware update and the vendor driver's NIC array. AP/WoWLAN arrays are
different operational roles, not a required T3U Plus-specific replacement.

## Bounded Linux diagnostic and restoration

The diagnostic supplied the verified blob and license in a temporary `/run`
directory, temporarily selected that firmware search path, and bound only the
target interface. A temporary USB-ID-specific udev rule kept its network
interface unmanaged. No AP connection or stored credential access was used.

Three bind attempts occurred, with two diagnostic-harness corrections:

1. The first High-Speed probe parsed firmware `30.20.0`, H2C `14`, then
   switched USB mode. The harness expected the old sysfs path and cleaned up
   before the SuperSpeed re-probe. Register `-71` messages coincided with the
   disconnect; the subsequent new-device probe again lacked temporary firmware.
   A firmware-path restoration newline discrepancy was explicitly corrected
   and verified before retrying.
2. A SuperSpeed bind succeeded and created a WLAN interface, but the harness
   stopped before bringing it up because its add-only udev rule lost the
   unmanaged property during interface rename. Cleanup completed.
3. The corrected add/change/move rule retained `NM_UNMANAGED=1`; NetworkManager
   reported `10 (unmanaged)`. Interface-up and one passive scan then succeeded.

Final successful cell:

```text
[191662.153802] rtw88_8822bu 4-4:1.0: Firmware version 30.20.0, H2C version 14
bound driver: rtw88_8822bu
scan: iw dev <target-interface> scan passive
scan exit status: 0
scan stderr: empty
link: Not connected.
```

| Frequency | Channel | BSS count |
| --- | --- | --- |
| 2412 MHz | 1 | 3 |
| 2437 MHz | 6 | 1 |
| 2442 MHz | 7 | 2 |
| 5220 MHz | 44 | 2 |
| Total | | 8 |

The fixture's supported-frequency text parser returned an empty list; this is
not used as radio-capability evidence. The successful scan frequencies above
are the directly observed result. No RFE/cut/EFUSE values were obtained:
the installed kernel has both `CONFIG_RTW88_DEBUG` and
`CONFIG_RTW88_DEBUGFS` unset.

Final restoration checks all passed: target driver unbound, original empty
firmware search path restored byte-for-byte, temporary udev rule and firmware
directory absent, permanent firmware file still absent, default routes
unchanged, SSH available. Uploaded diagnostic files were then removed from
the host. The adapter remains in SuperSpeed mode until a later hardware/mode
transition; the original High-Speed enumeration was not restored or claimed.

## Existing-code delta and acceptance limit

`src/drivers/usb-rtl8822bu.c` checks only Nano `012e` in both binding and its ID
table, accepts only High Speed, and fixes bulk MPS at 512. The private radio
transport currently carries no speed. `src/drivers/rtl8822b.c` always programs
the High-Speed RXDMA value `0x1e`; SuperSpeed requires `0x0e`. The RTL8822B v1
aggregation value remains `0x2005` at both speeds. A cut-D-specific USB3 PHY
adjustment and existing 512-byte TX-padding behavior need explicit review.

The local board parser supports RFE `2/3/5`. Its W52 transmission contract
requires country `JP`, channel plan `0x27` or `0x7f` and valid calibration.
Linux passive reception at channel 44 does not prove those conditions or
zedBSD transmission. No product-wide revision claim, zedBSD radio success,
WPA2/IP success, performance result or firmware replacement requirement follows
from this intake.

## Primary sources

- [Linux v6.19 exact USB ID table](https://github.com/torvalds/linux/blob/v6.19/drivers/net/wireless/realtek/rtw88/rtw8822bu.c)
- [Linux v6.19 RTL8822B firmware, RFE and USB PHY definitions](https://github.com/torvalds/linux/blob/v6.19/drivers/net/wireless/realtek/rtw88/rtw8822b.c)
- [Linux USB mode and RXDMA implementation](https://github.com/torvalds/linux/blob/v6.19/drivers/net/wireless/realtek/rtw88/usb.c)
- [Official firmware update to 30.20.0, 2025-11-10](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware.git/+/a50c068b4977be0845596e8294b47199ed2c5e2b)
- [Pinned vendor NIC firmware source](https://github.com/RinCat/RTL88x2BU-Linux-Driver/blob/0026128fa37398416bc14a3220e85596b84cf6c7/hal/rtl8822b/hal8822b_fw.c)
- [Pinned GitHub mirror WHENCE](https://github.com/endlessm/linux-firmware/blob/2f56219d20e4becccd718963fc3bcc671c543ce5/WHENCE)
- [Pinned Realtek license](https://github.com/endlessm/linux-firmware/blob/2f56219d20e4becccd718963fc3bcc671c543ce5/LICENCE.rtlwifi_firmware.txt)

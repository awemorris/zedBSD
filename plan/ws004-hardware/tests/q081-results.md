# Q081 Archer T3U Plus TX completion follow-up

Last updated: 2026-09-06

Phase: [ws004-p046](../phase046-archer-t3u-plus-driver/phase.md)

Status: finished; dual-band runtime passes, final post-5-GHz reopen remains for q082

## Starting evidence and finite scope

[Q080](q080-results.md) completed four bounded launches. The exact SuperSpeed
adapter binds and receives ordinary wireless frames, but no firmware C2H
notifications were parsed before three TX-report timeouts caused recovery.
The checked ON-section companion operation is implemented and verified but
does not alone resolve the hardware failure.

Q081 continues the user's authorized implementation toward useful traffic on
both supplied bands. Its first step compares hardware TX queue/endpoint
selection, management descriptors and firmware report setup with primary
upstream implementations. Only localized corrections and bounded nonsecret
diagnostics are in scope; suppressing TX reports or widening retries is not
an acceptance substitute. The existing private credential and host restoration
contracts remain in force.

Review after 90 active minutes, at most four new exact-device launches, each
at most 20 minutes. Every correction launch requires new evidence and an
actual change. Both-band normal-path completion remains the p046 criterion.

## Primary-source-localized corrections

The queue and endpoint audit found no mismatch: management selects HIGH/OUT05
and BE selects LOW/OUT08. Hardware page mapping and single-frame TX aggregation
match the upstream RTL8822B path.

Two independent reviews found startup/decoder omissions:

- GENERAL_INFO and PHYDM_INFO H2C messages are absent after fresh firmware
  startup. The existing layout reserves 52 pages with boundary 1996 and FW TX
  boundary 2044; the relative general-info value is 48. PHYDM fields come from
  the actual parsed board, including cut D/RFE3/2T2R on this unit. Each message
  occupies a 32-byte command buffer in an 80-byte USB H2C transfer, without a
  CCX request or a firmware acknowledgement wait. Reference:
  [Linux firmware commands](https://github.com/torvalds/linux/blob/v6.19/drivers/net/wireless/realtek/rtw88/fw.c#L504)
  and [vendor firmware initialization](https://github.com/RinCat/RTL88x2BU-Linux-Driver/blob/0026128fa37398416bc14a3220e85596b84cf6c7/hal/halmac/halmac_88xx/halmac_fw_88xx.c#L1047).
- C2H decoding applies ordinary-frame CRC/rate/PHY checks before delivering
  firmware notifications. Upstream returns before frame-status interpretation
  for C2H. The correction preserves all buffer/offset bounds and frame checks,
  while keeping notification metadata separate. Reference:
  [Linux receive decoder](https://github.com/torvalds/linux/blob/v6.19/drivers/net/wireless/realtek/rtw88/rx.c)
  and [vendor RTL8822B receive decoder](https://github.com/RinCat/RTL88x2BU-Linux-Driver/blob/0026128fa37398416bc14a3220e85596b84cf6c7/hal/rtl8822b/rtl8822b_ops.c).

These are source-supported functional corrections. Neither is claimed as the
hardware timeout cause until the next exact-device observation.

## Additional bounded observations

Before reset, a fixed six-register diagnostic records TXDMA status, HIGH queue
reserved/available pages, DMA queue mapping, TXPAUSE and CR. It reserves the
existing radio-operation slot or skips when busy, uses a shared 100-ms bound,
and does not update or replace the original recovery error.

The optional `--usbmon` runner observer reads only the target device's bulk
events and retains descriptor metadata, bounded sample lists and counters.
It discards all frame/command payloads and does not write raw monitor records.
The [kernel usbmon format](https://docs.kernel.org/usb/usbmon.html) defines the
fields; observations represent host-controller submissions/completions and
are not an over-the-air transmission proof. Synthetic checks cover target
filtering, C2H metadata, TX checksum, payload exclusion and sample caps.

## Automatic evidence before cell 1

The H2C startup and C2H decoder corrections each passed independent source
review. The combined USB driver fixture passes ordinary, ASan/UBSan with leak
detection, and GCC analyzer gates, including all three USB profiles, one/two
RF paths, exact startup bytes, both command failures, queue/deadline bounds,
checked rollback and reopen. The core gate passes the same three modes with
notification/frame paired cases, all encoded info/shift layouts, truncation,
aggregate atomicity and absent frame-metadata assertions.

The snapshot gate passes success, short/error, owner-busy and overall-timeout
cases while preserving original recovery state. USB descriptor observer review
corrected the management QSEL to decimal 18; synthetic management and H2C
samples now both pass. Remote observer open/stop without a guest also passes.

## Cell 1: correct host TX descriptors, no firmware report delivery

Runtime SHA-256:
`b0805b2892ca384b2881424575cbb9b5c31ac0c73750e6dada82e949d39505e3`.
All three configured x86 builds pass. The guest again recovered after three
missing reports, with 64 parsed frames and no C2H before recovery. The host
observer recorded 89 successful bulk OUT transfers and 111 normal first RX
descriptors across the complete cell, with no first-record C2H. Both startup
H2C descriptors and the three management descriptors had the expected queue,
offset and valid checksum; management reports were requested with sequences
0, 1 and 2. The implemented corrections do not alone resolve this failure.

TXDMA status `0210` read zero and HIGH reserved pages `0230` read 64. The
remaining snapshot fields expired because logging occurred inside its 100-ms
window. Cell 2 first collects all values and releases the operation reservation,
then prints. A regression fixture makes each diagnostic print consume 100 ms
and proves all reads precede output. Driver ordinary/sanitizer/analyzer gates
pass. No timeout/retry limit or hardware setting is changed by this correction.

Target presence/unbound state, management routes and image hashes pass after
cell 1; the credential-bearing guest copy was deleted. An additional bounded
Linux association baseline (at most 60 seconds per supplied band, no DHCP or
route changes) will distinguish a firmware/hardware TX problem from zedBSD
startup. It uses the same pin, runtime-only credentials and target-only driver
binding, restores host settings, and is not zedBSD acceptance evidence.

## Linux association baseline: both bands pass with the same firmware

The isolated supplicant reached WPA2-PSK/CCMP `COMPLETED` on 2412 MHz and
5220 MHz with the unchanged pinned firmware. No DHCP or route operation ran.
The host observer counted 12 C2H first descriptors; authentication/association
used endpoint 05, QSEL 18, offset 48, report requests and valid checksums.
This establishes usable TX and firmware reporting on the exact hardware and
pin under Linux. It narrows the zedBSD failure to local setup/progress, without
claiming zedBSD band acceptance.

Restoration verifies target unbound, exact original firmware search path,
temporary rule/firmware removal, absent permanent firmware, unchanged routes
and other network devices, and stopped isolated supplicant. The helper was
removed. Retained evidence contains only band labels/frequencies and descriptor
metadata, with no supplied values or frame payloads.

## Source-localized TRX and Wi-Fi-only startup profile

The path-A CDD/ADC selections incorrectly required exactly one RF path, while
upstream applies them whenever path A is present, including A+B. The three
conditions are corrected; core ordinary/sanitizer/analyzer gates pass RFE2 and
cut-D/RFE3 across one/two RF paths, both USB speeds, band round trips and each
write failure. [Linux TRX setup](https://github.com/torvalds/linux/blob/v6.19/drivers/net/wireless/realtek/rtw88/rtw8822b.c#L771)
provides the hardware semantics.

Independent review also established that upstream initializes a Wi-Fi-only
board's WLAN grants and antenna ownership even without Bluetooth coexistence.
The local path omits these operations. The narrow profile applies only to a
known RF board option (`!= ff`, upper bits not `20`), retaining behavior for
unknown/BT-capable boards. It sets indirect grant register `38` mask `ff80` to
`7700`, WLAN owner `73` bit 2, and BB DPDT owner `4c` mask `01800000` to
`01000000`. The existing `radio_bb_channel_20` journal already sets `cbc`
mask `300` to `200` on 2.4 GHz or `100` on W52, and later RFE updates preserve
those bits. The initial review missed this earlier operation; the redundant
proposed band write was removed before freezing the implementation. Band
round-trip and rollback checks remain applicable. Unrelated fields are preserved.
The attach diagnostic now includes the actual nonsecret RF board option;
applicability to this unit is to be verified on the next launch.

Primary sources: [vendor minimal Wi-Fi-only initialization and band switch](https://github.com/RinCat/RTL88x2BU-Linux-Driver/blob/0026128fa37398416bc14a3220e85596b84cf6c7/hal/btc/halbtc8822bwifionly.c)
and [Linux antenna ownership](https://github.com/torvalds/linux/blob/v6.19/drivers/net/wireless/realtek/rtw88/rtw8822b.c#L1146).
Checked indirect readiness/readback, startup unwind and journal rollback are
required. No PTA/TDMA/Bluetooth feature or RF power workaround is introduced.

The final profile passes independent review and core ordinary, sanitizer/leak
and analyzer gates. Coverage includes parsed cut-D/RFE3 board options `00`,
`01`, `ff`, `20`, `3f`, unrelated-bit preservation, band round trips, six
startup write failures, seven indirect read failures, ineffective grant/owner/
DPDT writes, the fixed busy-poll bound and the shared deadline. Readbacks must
succeed before the initial channel and final TX unpause.

## Cell 2: scan, 2.4-GHz authorization, DHCP and ping pass

Runtime SHA-256:
`2689cb9e3a56f16995673cde7c8e61b3ed9a11eebfcec5cc7edbebebc6bea724`.
The final core/driver ordinary, sanitizer/leak and analyzer gates and serialized
configured amd64, PC/AT and PC-98 builds pass. Attach measured RF board option
`01`, confirming this unit uses the implemented Wi-Fi-only profile.

The complete scan returned ten BSS records. Ordinary `net wifi connect`
completed 2.4-GHz WPA2/CCMP authorization and DHCP; all three ICMP probes to
the host returned. The earlier missing-TX-report failure did not recur.

HTTP payload transfer then triggered recovery with `EALREADY` (54), three RX
error streak entries and zero TX tombstones. Before recovery, the driver had
743 USB completions, 46 C2H and 727 wireless frames. The repaired snapshot
collected every value successfully: TXDMA status zero, HIGH reserved/available
64/64, queue map `f5a5`, TXPAUSE zero and CR `06ff`. Both-band data/lifecycle
acceptance is still incomplete; 5 GHz was not attempted in this cell.

Source tracing localizes 54 to the common L2 CCMP duplicate/non-increasing PN
guard. That expected per-frame rejection is incorrectly propagated into the
driver's USB-recovery streak and aborts later aggregate records. Cell 3 will
absorb only `EALREADY` from DATA/EAPOL frame delivery, preserving the PN guard
and all USB, structural aggregate, management and CCX fault behavior. The
same expected duplicate result occurs for already-pending EAPOL messages.
No QoS, replay-window widening or retry-limit change is required.

The host/device/image restoration checks all pass, and the credential-bearing
guest was deleted. New evidence narrows the remaining work to receive
continuation during useful data traffic and the still-unrun 5-GHz normal path.

The scoped DATA/EAPOL duplicate-drop correction passes driver ordinary,
sanitizer/leak and analyzer gates. Tests reproduce three duplicate-only RX
completions at small and large frame sizes without recovery/reload, and retain
a fresh second record in the same aggregate. Management/CCX and other error
paths remain unsuppressed. The unchanged common L2 replay guard separately
passes its existing ordinary, sanitizer and analyzer gate.

## Cell 3: 2.4-GHz full path passes; reopen scan hits control timeout

Runtime SHA-256:
`57012a7643c52d35109c35968a66c761fcdc89d318ff709e78bb59d59b47a212`.
All three configured builds and the scoped duplicate-drop review/gates pass.
The 2.4-GHz channel-1 path completed normal WPA2/CCMP authorization, DHCP,
three successful ICMP replies, a 4480-byte HTTP transfer with matching POSIX
checksum, disconnect and administrative down. The duplicate-frame recovery
failure did not recur.

After administrative reopen, the scan reached channel 8 and its ON-register
companion write timed out with error 42 and actual length zero. The channel
operation still had 43 ticks (430 ms) left. Checked channel failure stopped
the radio and quarantined the adapter; read-only status then returned ENETDOWN,
hiding the common layer's terminal scan failure. The harness ended after its
90-second scan bound. No 5-GHz connection was attempted. Host/device/image
restoration passed and the credential-bearing guest was deleted.

Primary comparison establishes that the local 20-ms register/companion limit
is far shorter than upstream: [Linux USB registers](https://github.com/torvalds/linux/blob/v6.19/drivers/net/wireless/realtek/rtw88/usb.c)
use 500 ms for writes/companions and 1000 ms for reads; the [vendor common limit](https://github.com/RinCat/RTL88x2BU-Linux-Driver/blob/0026128fa37398416bc14a3220e85596b84cf6c7/include/usb_ops_linux.h#L21)
is 500 ms. Console/VM scheduling delay is a possible trigger, not a proven
cause. The correction uses the vendor 500-ms cap while propagating each radio
operation's absolute deadline into every primary/companion/retry transfer.
Ambiguous timeouts are not retried; unrelated deauthentication and diagnostic
budgets retain their existing limits.

The scan-only, checked-off failure path will schedule the existing bounded
device recovery worker once. Failed restart remains quarantined. Read-only
scan/status/BSS queries remain available so the terminal error can be observed;
mutations and disconnected-device behavior stay guarded. The host observer
adds request-address/width/status/latency metadata for control transfers,
without decoding or retaining their data, to measure timing on the next run.

The deadline/recovery changes pass the combined USB driver ordinary,
ASan/UBSan/leak and GCC analyzer gates and an independent review. Radio core
and security gates pass the same checks. The transport recomputes the remaining
budget before each submission and rejects late success. Existing USB-core
control-lock acquisition and cancellation/drain can extend teardown beyond
the caller deadline; this is not a strict wall-clock completion guarantee.
All three configured x86 builds pass before preparing cell 4.

## Cell 4: deadline-aware transfers and checked scan recovery

Runtime SHA-256:
`f9d74fbdcfd2860571358739a1a7bd239ffa5359e0c534b5803cdbd6145c46b7`.
Production image SHA-256:
`12c7030e255394451fa51eff9ec152ccc9d7ff0eeef27853096450ce94b47890`.
The embedded fixture is unchanged and its digest is checked after installation.
The exact SuperSpeed adapter passed both ordinary user connection flows:

| Band | Channel / frequency | WPA2-PSK/CCMP | DHCP | ICMP | HTTP payload / POSIX checksum | Disconnect / down |
| --- | --- | --- | --- | --- | --- | --- |
| 2.4 GHz | 1 / 2412 MHz | authorized | pass | 3/3 | 4480 bytes / `4004478192` | pass |
| 5 GHz | 44 / 5220 MHz | authorized | pass | 3/3 | 4480 bytes / `4004478192` | pass |

Reopen and a fresh scan between the two bands passed. No control-transfer
error, scan-channel failure or recovery diagnostic appeared. The observer
counted 114 first-record C2H notifications and 1328 first-record frames;
both CCK and OFDM management descriptors had valid checksums and checked
report requests. All 22,755 observed vendor controls completed successfully,
with a largest host submission/completion interval of 268 microseconds.
This host measurement excludes guest interrupt/scheduling delay and cannot
establish the exact cause of the preceding cell's 20-ms guest timeout.

Target presence/unbound state, management routes and input hashes all pass;
the credential-bearing guest image was deleted. The runtime acceptance limit
is this actual SuperSpeed unit and 20-MHz W52 channel 44. Other allowed W52
channels, physical High-Speed operation, DFS, wider channels and throughput
qualification are not claimed.

Q081 used its four-launch budget. The fixture ended immediately after the
5-GHz administrative-down check; it did not reopen after that final stop.
P046 explicitly requires this lifecycle observation, so the phase remains
uncleared only for that gap. Q082 adds the missing final enable/scan/down
sequence in one bounded launch with the same production image. A harness-only
review also found that an exception in observer/restoration diagnostics could
skip temporary-image deletion; the closure fixture makes cleanup independent
of those diagnostics. Neither change alters production driver behavior.

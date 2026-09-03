# Queue: amd64 HAL counter and AX211 association closure

Last updated: 2026-09-03

QID: `q066`

Queue status: finished

Queue finished: **Yes**

Authorization: the user requested a Queue that implements
`hal_rtc_read_counter()` with working AX211 as the end goal, then explicitly
approved execution. Runtime and focused testing is limited to amd64 and i386;
arm64, sparcv9, and X68000 require successful configured builds only.

Proposed timebox: none. Execute the two finite Phases below in dependency order.

Parent: [master plan](master.md)

Previous Queue: [q065](queue-q065.md)

## Purpose

Provide the agreed HAL wall-clock/counter split and a proven amd64
fixed-frequency monotonic counter, then use that platform service to finish the
exact AX211/CNVio2 useful network path. This Queue fixes both independently
observed prerequisites: a proper HAL deadline source and the pinned `-89`
firmware's MLD association-command family.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p025` | [HAL clock-source split and amd64 SMP monotonic counter](ws003-bringup/phase025-amd64-smp-counter-validation/phase.md) | uncleared; automatic milestone complete, one shared direct-boot observation pending | The approved two-function HAL API is complete; amd64 calibrates privately and publishes its raw TSC counter only after the whole admitted CPU set passes, with QEMU SMP/negative/build evidence |
| 2 | `ws004-p038` | [standalone AX211 normal path](ws004-hardware/phase038-intel-ax211-standalone-driver/phase.md) | uncleared; automatic and exact-device VFIO normal-path milestones complete, one shared direct boot pending | The API89 MLD family and focused gates pass; the exact device reaches WPA2/DHCP/ping/fetch/down with guaranteed host restoration |

## Accepted design decisions

- The only public time-source operations are
  `hal_rtc_read_epoch_time(uint64_t *)` and
  `hal_rtc_read_counter(uint64_t *, uint64_t *)`.
- A successful counter read returns a boot-local raw fixed-frequency sample and
  its nonzero frequency. The epoch is unspecified, frequency is stable, and
  samples successfully returned to callers never go backward across CPU
  migration. `false` leaves output arguments unchanged.
- TSC selection, CPUID.15 or bounded-PIT calibration, publication state, and
  SMP validation belong to private amd64 HAL clock/timecounter and startup
  code. The generic kernel and AX211 do not calibrate or correct TSC.
- Every admitted CPU must pass. Missing invariant TSC, incompatible frequency
  metadata, ambiguous bracket samples, inconsistent `IA32_TSC_ADJUST`, or a
  runtime backward sample makes the counter unavailable. Do not write TSC,
  compensate per CPU, or clamp returned values.
- Other architectures retain truthful implementations: architectural counters
  where already available, and `false` where no conforming counter exists.
- The counter is necessary for AX211 deadlines but is not the observed cause of
  the association assert. The exact `-89` firmware remains pinned and its
  advertised MLD API is implemented as one family: `MAC_CONFIG`,
  `LINK_CONFIG`, `STA_CONFIG`, and its matching key path. Do not retain
  `MAC_CONTEXT`/binding/legacy station commands in that path and do not replace
  the firmware with `-77`.
- No further public HAL or WLAN UAPI is introduced. Any need to write TSC,
  compensate offsets, clamp samples, change firmware, or alter a public API
  stops the affected Phase for human review.
- Runtime Wi-Fi credentials stay only in ignored `.internal/` material. They
  must not enter M/W/P/Q, tracked tests, command arguments, or retained logs.

## Verification order

1. Run private frequency/SMP policy fixtures, including missing features,
   mismatched CPUID data, TSC-adjust mismatch, arithmetic limits, delayed or
   ambiguous AP samples, and runtime backward fail-closed behavior.
2. Run an amd64 UEFI SMP QEMU counter stress plus a negative injected-skew cell;
   retain existing early-init and AX211 PCI-MMIO focused gates.
3. Complete the exact API89 MLD command encoders/state transitions and their
   byte-exact, rollback, malformed-event, security, TX/RX, and build tests.
4. Run `make -j16`, focused amd64/i386 regressions, and configured amd64/i386
   build gates. For arm64, sparcv9, and X68000, require configured builds only;
   do not add runtime or focused execution gates. Do not run aggregate
   `make check`.
5. Use the bounded exact-device VFIO runner through scan, WPA2/CCMP, DHCP,
   gateway/public ping, bounded nonempty fetch, disconnect, and down. On every
   exit, restore `0000:00:14.3` to `iwlwifi` and verify the independent SSH
   route.
6. Ask for one final direct zedBSD boot only after automatic and VFIO work is
   otherwise complete. That one run supplies both p025's physical multicore
   counter observation and p038's native AX211 acceptance; repeated human boots
   are not intermediate blockers.

## Completion definition

Q066 finishes when both items are completed or honestly uncleared with exact
facts and resume conditions, host AX211 ownership is restored after every VFIO
attempt, temporary secret-bearing artifacts are removed, and P/W/M/Q agree.
The Queue may finish before p025/p038 only if the remaining condition is the
single combined direct-boot result or a newly identified human decision.

## Execution result

- `ws003-p025` completed its approved public HAL split, private amd64 counter
  policy, whole-admitted-CPU publication barrier, positive/negative SMP QEMU
  evidence, and configured build matrix. It remains uncleared only for the one
  physical multicore observation shared with p038's final direct boot.
- `ws004-p038` replaced the legacy/API89 mismatch and corrected the dynamic TX
  queue/completion contract. The exact AX211 then passed firmware/PNVM start,
  5-GHz scan, target discovery, WPA2/CCMP association, controlled-port
  authorization, DHCP and default-route installation. The user-selected LAN
  peer at `10.0.10.3`, a public ping, a bounded nonempty HTTP fetch,
  disconnect, and administrative down all passed in one bounded VFIO run.
- The common L2 receive path now accepts ordinary TID-0 non-A-MSDU QoS Data, with
  checked QoS/optional-HT/CCMP offsets. Its focused ordinary, sanitizer, and
  analyzer gate passes, and the same production image passed the useful-IP
  run. Full WMM/nonzero-TID policy, A-MSDU, and exhaustive QoS error handling remain
  outside this normal-path Queue.
- The VFIO controller and runner both returned success. The exact BDF was
  restored to `iwlwifi`, the independent Ethernet route remained available,
  and the remote disposable image, logs, and credential-bearing staging tree
  were deleted. No SSID, BSSID, MAC address, or passphrase is retained here.

Q066 therefore finishes under its explicit completion exception. P025 and
p038 remain uncleared solely for their one shared direct-boot observation;
neither Phase is incorrectly claimed complete from a virtualized host path.

# WS004: hardware expansion

Last updated: 2026-09-01

WSID: `ws004`

Status: active; WS is not complete. q041 completed p016 checked legacy-HCD
request retirement. q047 completed p031 legacy-HCD concurrent scheduling and
root hotplug plus p032 checked endpoint/device recovery. P033's
extracted amd64 framebuffer-console correction is complete after passing its
host gates and the shared forced HW-T25 QEMU matrix. P031 and p032 are
complete, q048 completed WS006 p008's automatic USB HID milestone, and q049
completed p019's independent CDC ECM QEMU network baseline. `ws004-p026` is
complete after q055 recorded the Japan-market label, explicit absence of a
printed revision, descriptor-authority decision, and optional firmware-package
boundary. `ws004-p020` deterministic CDC NCM hardening and
`ws004-p022`--`p024` NVMe discovery/I/O/strict-GPT QEMU acceptance are
complete; q052 has completed `ws004-p021`'s current-source automatic/runtime
milestone, including one fresh xHCI USB-root QEMU boot, and the Phase is
uncleared only at one hash-pinned Latitude/RTL8156 checkpoint;
q054 completed `ws004-p017`'s shared asynchronous-TX statistics helper and
exactly-once CDC NCM terminal accounting;
`ws004-p027`--`p030` define the Archer T3U Nano WLAN implementation path;
q055 completed `p027`, q056 completed p036's independently testable RTL8822BU
pre-radio substrate, q057 completed p028's automatic radio/scan milestone, and
q058 completed p029's automatic secure-L2 milestone. q059 now moves to WS005
p004/p009 to establish one simple direct-command physical communication path;
p030 lifecycle hardening is deliberately deferred until that path works. `p034`
separately records deferred, nonblocking CDC ECM accounting adoption, and
`p035` records the nonblocking future same-endpoint multi-URB extension which
the first WLAN scan path deliberately does not require.

Parent: [master plan](../master.md)

Last verified Phases: `ws004-p010` completes the retained USB function model;
`ws004-p011` completes concurrent xHCI endpoint ownership and callback drain;
`ws004-p012` completes removable network-device and terminal shutdown lifetime;
`ws004-p013` completes the strict, bounded NTH16/NDP16 wire contract; `p015`
completes the general USB binding/interface transaction; and `p014` completes
the integrated `ueN` automatic software milestone.
`p020` completes the deterministic valid-sequence/resynchronization,
completion-budget, rearm, and packet-filter-open hardening with focused and
regression evidence. The q029 physical follow-up captured valid RTL8156
connection/speed notifications with data-interface `wIndex`; the repaired NCM
parser and real adapter pass carrier, DHCP, and ping through QEMU xHCI, and the
final Latitude-native image successfully fetches `www.google.com`.
`p021` corrects the independent native-xHCI Max ESIT defect exposed by the same
audit; q045 passes its exact-word, strict-validation, regression, analyzer, and
configured-object gates. Q047's accepted Noct `--path` runtime released the
candidate build, and q052 then passed the current-source regressions, fresh
private configured build, and OVMF q35/xHCI USB-root boot. One physical
Latitude checkpoint remains. `p016`
now completes the controller-proven
legacy-HCD hardware-retirement follow-up exposed by that audit. `p031` now
completes concurrent UHCI/EHCI endpoint ownership, request-local retirement,
shared-INTx dispatch, and worker-context root hotplug through all focused,
configured-build, regression, repository-build, and two-cell QEMU gates. `p018`
corrected the IAD-less, Union-associated NCM match exposed by
the first physical RTL8156 insertion, and the accepted follow-up published
`ue0`. Q054 resolves the asynchronous-TX/accounting boundary: successful
driver acceptance owns packet/byte counters, later genuine NCM terminal
failure adds one error only, and administrative cancellation adds none. Its
focused, sanitizer, analyzer, configured-build, retained-regression, and fresh
OVMF q35/xHCI USB-root gates pass.

Resume point: p031, p032, and p033 are complete, q048 consumed their USB HID
handoff, and q049 completed p019's automatic ECM and four-cell QEMU scope. No
physical-machine recovery result is claimed by p032 or p019.
The single non-reproduced UHCI active-list `#GP` observed during q048 remains
open as [`BUG-008`](../known-bugs.md); recurrence promotes it to a finite WS004
Phase and reopens the legacy-HID acceptance rather than changing p031 by
speculation.
The 2026-08-30 completion audit also found p017, p021, p025,
and p026--p030 still open. q030 completed p022 through p024, including strict
primary/backup GPT and the final disposable QEMU acceptance matrix. The later
p025 is the single read-only Latitude SN740 checkpoint. Complete the remaining
automatic loader/installer prerequisites before requesting that physical
checkpoint. Q052 consumed q047's Noct release and completed p021's disposable
QEMU boot; one hash-pinned Latitude checkpoint remains. P019 completed in
q049 as an independent ECM baseline. The remaining asynchronous-TX/accounting
portion of p017 completed in q054. CDC ECM adoption of the same helper is now
recorded as the planned/deferred, nonblocking
[`p034`](phase034-cdc-ecm-async-tx-accounting/phase.md) rather than reopening
NCM. WLAN planning resumed
with the Archer T3U Nano as the first target. q055 closes `p026`: the purchased
Japan-market unit has no printed revision, so its retained exact USB descriptor
is the binding authority, and firmware is acquired only through the separately
installed optional `rtl8822b-firmware` entry under `userland/firmware/`. Q055 completed `p027`'s generic
kernel WLAN core and its x86/QEMU gates. Q056 completed p036's default-off
firmware package, USB/register/efuse/firmware/RX substrate, and p027
integration before radio-table programming. Q057 completed p028's
notice-preserving BSD-3-Clause table import, binary notice installation,
bounded radio initialization, and conservative RTL8822BU scan. Q058 completed
the WPA2-Personal/CCMP L2 milestone. WS005 p009 is the next developmental
checkpoint; p030 remains the later lifecycle-hardening successor.

Shared tests: [WS004 test index](tests/README.md)

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws004-p001`](phase001-foundation-audit/phase.md) | Complete | Software audit and two common fixes pass; Latitude evidence remains in `ws003-p001` |
| [`ws004-p002`](phase002-pcie-xhci-prerequisites/phase.md) | Complete software milestone | Config limits, extended capabilities, bridge-tree scan, and contract evidence pass |
| [`ws004-p003`](phase003-ecam-msi/phase.md) | Complete software milestone | Fixed HAL API, MCFG/ECAM, amd64 vector allocation, PCI MSI/MSI-X lifecycle, and real QEMU delivery pass; unavailable cross-target toolchains are recorded |
| [`ws004-p004`](phase004-xhci/phase.md) | Complete QEMU storage milestone | Native xHCI uses MSI-X, enumerates and reads USB storage, and passes disconnect/reconnect; SuperSpeed/fault injection and Latitude evidence remain follow-ups |
| [`ws004-p005`](phase005-usb-root/phase.md) | Partial; automatic runtime cleared | BIOS/UEFI identity and revised 500-boot writable-root gate pass; malformed-handoff fixture and detailed manual acceptance remain |
| [`ws004-p006`](phase006-usb-overlay-write/phase.md) | Complete automatic QEMU milestone | URB correction plus p008 heap fix pass focused tests and the revised 500-boot gate; manual acceptance is pending |
| [`ws004-p007`](phase007-warm-reset/phase.md) | Complete | Native-mode ELF64 BSS clearing fixes stale allocator state; three IDE reboots and a USB reboot reach login |
| [`ws004-p008`](phase008-smp-heap-integrity/phase.md) | Complete | Unified kernel heap lock domain, corrected aligned-prefix arithmetic, controls, and 500-boot combined gate pass |
| [`ws004-p009`](phase009-pci-hcd-irq-teardown/phase.md) | Complete (`q016`) | EHCI/UHCI use checked quiesce, retain all ownership on failure, restore staged attach/detach state, unlink stale root-probe nodes, pass HW-T02 and both i386 production builds |
| [`ws004-p010`](phase010-usb-function-model/phase.md) | Complete (`q027`) | Retained multi-configuration/alternate/IAD/extras and UTF-8 strings, deterministic driver-aware configuration choice, exclusive sibling claims, and range-limited checked endpoint rollback pass 1280 focused checks and configured x86 builds |
| [`ws004-p011`](phase011-xhci-concurrent-urbs/phase.md) | Complete (`q027`) | Per-endpoint xHCI ownership, exact event claim, checked cancel/drain, bounded reclaim reserve, opaque concurrency capability, and callback-aware URB drain pass focused, analyzer, configured-build, and USB-root QEMU gates |
| [`ws004-p012`](phase012-net-device-hotplug/phase.md) | Complete (`q027`) | Safe carrier, concurrent hot-unplug barriers, deferred slot release, stale identity purge, and terminal shutdown lifetime pass focused, sanitizer, and 500-run race gates |
| [`ws004-p013`](phase013-cdc-ncm-wire/phase.md) | Complete (`q027`) | Strict NCM 1.0-compatible NTH16/NDP16 negotiation and wire codec pass ordinary, sanitizer, analyzer, and production-build gates |
| [`ws004-p014`](phase014-cdc-ncm-driver/phase.md) | Complete (`q027`) | Strict self-contained CDC NCM `ueN` integration passes automatic lifecycle, concurrency, build, and QEMU regression gates; physical NCM remains WS005 |
| [`ws004-p015`](phase015-usb-binding-transactions/phase.md) | Complete (`q027`) | General binding lifecycle, interface I/O gate, active-endpoint submission, endpoint-zero serialization, and conservative legacy-HCD ownership passed focused, sanitizer, analyzer, build, and USB-root QEMU gates |
| [`ws004-p016`](phase016-legacy-hcd-request-retirement/phase.md) | Complete (`q041`) | Controller-proven UHCI frame and EHCI Async Advance retirement, callback re-entry and toggle continuity pass focused/configured/QEMU gates; unavailable fault injection is explicitly model-only |
| [`ws004-p017`](phase017-cdc-ncm-runtime-recovery/phase.md) | Complete (`q054`) | Accepted packet/byte counters remain, genuine asynchronous NCM terminal failure adds exactly one locked `tx_errors`, administrative cancellation adds none, and focused/sanitizer/analyzer/build/OVMF xHCI USB-root gates pass |
| [`ws004-p018`](phase018-rtl8156-ncm-association/phase.md) | Complete (`q028`) | CDC Union is authoritative and IAD is optional strict corroboration; automatic gates pass and physical RTL8156 configuration 2 binds and publishes `ue0`; carrier/data work moved to WS005 p001 |
| [`ws004-p019`](phase019-cdc-ecm-qemu-baseline/phase.md) | Complete (`q049`) | Independent standards CDC ECM, the general xHCI/EHCI/UHCI zero-packet HCD contract, focused lifetime/fault evidence, and all four IDE/xHCI-storage static/DHCP QEMU cells pass without NCM wire sharing or VID:PID/configuration quirks |
| [`ws004-p020`](phase020-cdc-ncm-deterministic-hardening/phase.md) | Complete (`q029` automatic software scope) | Valid sequences accept/resynchronize, malformed input preserves state, completions and rearms are bounded/fair, and the packet filter is programmed transactionally on open; focused and regression gates pass |
| [`ws004-p021`](phase021-xhci-superspeed-interrupt-context/phase.md) | Uncleared (`q052`; automatic/runtime milestone passed) | Host-endian companion access, exact RTL8156 Max ESIT/Average TRB fields, pre-ring strict rejection, current regressions/x86 objects, fresh configured build, and one OVMF xHCI USB-root boot pass; only the hash-pinned Latitude carrier/DHCP/ping/fetch checkpoint remains |
| [`ws004-p022`](phase022-nvme-admin-identify/phase.md) | Complete (`q030`) | Bounded reset/admin Identify, transactional PCI/MSI lifecycle, stable names, focused fixtures, amd64/i386 builds, exact non-mutating QEMU namespace, IDE, and USB-root gates pass |
| [`ws004-p023`](phase023-nvme-io-lifecycle/phase.md) | Complete (`q030`) | One depth-64 I/O queue, private 4-KiB bounce slots, checked 64-bit read/write, truthful flush, concurrent wrap, timeout/reset, normal shutdown, and quarantine pass focused/build/QEMU/regression gates |
| [`ws004-p024`](phase024-nvme-qemu-acceptance/phase.md) | Complete (`q030`) | Strict 512/4096 GPT host gates and disposable QEMU partition write/flush/restart/rejection plus IDE, xHCI USB-root, amd64, and i386 gates pass |
| [`ws004-p025`](phase025-latitude-nvme-readonly/phase.md) | Planned physical checkpoint; depends on p024 | Latitude SN740 `15b7:5015` identifies and reads safely without modifying internal storage |
| [`ws004-p026`](phase026-archer-t3u-nano-identity-firmware/phase.md) | Complete (`q055`; q040 intake retained) | The Japan-market label has no printed revision; the retained exact `2357:012e` descriptor is authoritative, and the pinned upstream bytes plus explicit GitHub-mirror `rtl8822b-firmware` boundary are frozen |
| [`ws004-p027`](phase027-wlan-uapi-common-core/phase.md) | Complete (`q055`) | Versioned pointer-free WLAN ioctls, strict INET dispatch, persistent station/cache/generations, checked lifetime barriers, deterministic fake radio, x86 builds, and IDE/xHCI exact-login gates pass without a hardware claim |
| [`ws004-p028`](phase028-rtl8822bu-usb-scan/phase.md) | Automatic milestone complete (`q057`); shared physical feedback deferred | Pinned BSD-3-Clause tables and binary notice, checked power/MAC/PHY/USB profiles, ch1--11 20-MHz passive/wildcard-active production scans, fail-close lifetime, focused/build/IDE/xHCI gates pass; physical evidence remains in WS005 p008 |
| [`ws004-p029`](phase029-wpa2-ccmp-l2/phase.md) | Automatic milestone complete (`q058`); physical secure-L2 evidence deferred to the shared checkpoint | Common-kernel WPA2-Personal/CCMP authentication, association, four-way handshake, key CAM, controlled port, and bidirectional Ethernet L2; physical evidence is shared with p030/WS005 p008 |
| [`ws004-p030`](phase030-wlan-lifecycle-hardware-hardening/phase.md) | Planned/deferred; not queued; begins after WS005 p009 | Rekey, bounded reconnect, reset, up/down, unplug/reinsert, shutdown, and concurrent-storage regressions; share one lifecycle checkpoint and the frozen-artifact five-run ledger with WS005 p008 rather than duplicate physical work |
| [`ws004-p031`](phase031-legacy-hcd-concurrent-hotplug/phase.md) | Complete (`q047`) | UHCI/EHCI per-endpoint concurrency, periodic/asynchronous progress, request-local retirement, worker-context root hotplug, shared-INTx dispatch, all focused/configured/regression/build gates, and both forced QEMU cells pass |
| [`ws004-p032`](phase032-usb-endpoint-device-recovery/phase.md) | Complete (`q047`) | Ordered endpoint clear-halt, conservative direct-root reset, allocation-free reclaim-safe recovery, and Mass Storage migration pass HW-T26 and xHCI/legacy QEMU controls; physical recovery was not exercised |
| [`ws004-p033`](phase033-amd64-framebuffer-console-serialization/phase.md) | Complete (`q047`) | One early-safe lock and strict cell/framebuffer bounds pass HW-T27 host/sanitizer/input/build gates and the shared forced HW-T25 QEMU matrix without console fault or stall |
| [`ws004-p034`](phase034-cdc-ecm-async-tx-accounting/phase.md) | Planned/deferred; nonblocking; not queued | Apply q054's exactly-once asynchronous TX-error accounting to CDC ECM, preserve accepted packet/byte and drop meanings, and rerun automatic ECM gates without a physical check |
| [`ws004-p035`](phase035-usb-same-endpoint-multi-urb/phase.md) | Planned/deferred; nonblocking; not queued | Add a bounded same-endpoint xHCI URB ring only when a measured workload needs it; p028 retains one persistent bulk-IN URB and does not depend on this Phase |
| [`ws004-p036`](phase036-rtl8822bu-pre-radio-substrate/phase.md) | Complete (`q056`) | The default-off firmware package, exact USB/register/efuse/firmware/RX substrate, serialized p027 publication, fake DDMA/RX integration, and tableless production refusal pass before p028 programs RF |

### MSI follow-up register

| Item | Initial `ws004-p003` treatment | Resume condition |
| --- | --- | --- |
| Multi-message conventional MSI | Deferred; one registration and one message only | A consumer requires multiple power-of-two messages and has focused ordering tests |
| Dynamic MSI affinity | Deferred; mappings target CPU 0 | A public remap operation can return a replacement address/event pair safely |
| arm64 IORT/GIC ITS backend | Deferred; public signature preserved and port returns unsupported | An arm64 PCIe platform Phase supplies firmware and interrupt-controller fixtures |
| Non-PCI message source prefixes | Deferred; only canonical PCI BDF is accepted | A concrete platform device needs message interrupts and defines stable source identity |

q029, p020, p017/q054, and WS005 p001 are complete through final
Latitude-native external
fetch. p021 is an independent standards correction rather than an active
failure response; q052 retains its passing fresh-image QEMU result and only one
physical checkpoint remains. Later WS004 boundaries after q055 are the p021
physical checkpoint, p025, and the p028--p030 WLAN implementation chain; p026,
p027, p036, and q047 p031--p033 are complete, with p028's automatic milestone
complete in q057.
P034 and
p035 are separate deferred, nonblocking consistency/performance follow-ups.
Additional work is HW-11,
HW-20/HW-21, and HW-30 when their inputs and acceptance environments are
available. Q040 selected the evidence/policy-only p026 intake. Q055 closed its
later identity/package decision and completed p027; q056 completed p036 and
q057 completed p028's automatic milestone and q058 completed p029. q059 first
establishes the WS005 p004/p009 direct physical communication path; p030 and
P034 remain unqueued.

## Goals

- Provide the reusable PCIe, DMA, interrupt, reset, and firmware foundations
  required by the target laptop.
- Implement xHCI/USB-root, USB Ethernet, NVMe, the selected WLAN device, and
  i915 foundations as native zedBSD drivers.
- Keep modeled/QEMU results separate from physical-hardware results.

## WS completion conditions

WS004 is complete when the common hardware facilities pass focused regression
tests and the selected xHCI, NVMe, and WLAN driver scopes pass their declared
lifecycle and recovery tests on the Latitude 5320. Unsupported devices and
firmware constraints must be explicitly documented. Native GPU/i915 ownership
and its completion gate belong to pending WS014 rather than blocking WS004.

Primary physical target: Dell Latitude 5320

## 1. Objective

Build the reusable kernel foundations and native drivers needed for the target
laptop, beginning with xHCI/USB-root support and NVMe. Bring up the
descriptor-confirmed USB WLAN adapter through a reusable WLAN core before the
separate built-in PCI WLAN follow-up, then continue to the i915 graphics
generation discovered by hardware inventory.

## 2. Shared foundations

The driver work must audit and, where necessary, harden these common facilities
before individual drivers duplicate them:

- PCI/PCIe enumeration, BAR mapping, capabilities, and power state;
- legacy interrupts, MSI, and MSI-X with teardown and error handling;
- DMA allocation/mapping, address-width constraints, ordering, and cache
  coherency;
- bounded waits, controller reset, cancellation, and device removal;
- ACPI/firmware data used by the target devices;
- firmware-blob loading policy, provenance, versioning, and failure messages;
- block and network queue integration under concurrency.

IOMMU support is evaluated from the target firmware and DMA model. If it is not
implemented initially, the security and addressability limitation is explicit.

## 3. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| HW-00 | Complete (software scope) | PCIe/DMA/interrupt capability audit and common fixes | BR-00 hardware inventory deferred | Focused host tests and amd64 build pass; physical findings remain separate |
| HW-01 | Complete read-only QEMU milestone | xHCI host-controller support sufficient for storage and future HID | HW-00, existing USB core | QEMU xHCI enumeration, bounded media read, and reconnect pass; writable-root correctness is HW-02/p006 |
| HW-02 | Complete automatic QEMU milestone; manual acceptance pending | Stable USB identity/discovery, writable overlay, bounded read-only rejection, and warm reboot | HW-01, block layer, approved selector decision | Revised HW-T12 500-copy gate passes; record the user's detailed manual acceptance separately before physical USB-root claims |
| HW-03 | Complete (`q016`) | Checked PCI IRQ/controller lifetime for EHCI and UHCI detach; xHCI is already converted and is outside this Phase | HW-00, checked PCI IRQ API from q015 | Busy/error removal retains all ownership; retry and final detach host fixtures plus i386 builds pass |
| HW-10 | Complete (`q030`) | NVMe controller, admin/I/O queues, namespace naming, block integration, and disposable QEMU acceptance | HW-00 | QEMU NVMe identify/read/write/flush/concurrency/reset and GPT partition tests pass |
| HW-11 | Planned as `ws004-p025` plus WS003 p018 | Read-only SN740 verification before the separately confirmed existing-FAT overlay install/boot acceptance | HW-10, BR-00, WS019 | Identify/read-only first; file writes occur only through the explicitly confirmed installer Phase |
| HW-12 | Complete (`q054`; physical data previously accepted in WS005) | Independent CDC NCM class driver on the common USB/xHCI/net-device lifetime foundations; Union-associated NCM does not require an IAD; deterministic recovery, bounded work, transactional open filtering, and exactly-once asynchronous TX error accounting are complete | HW-01 | p020 and p017 automatic gates pass; the Latitude RTL8156 carrier/DHCP/ping/fetch result remains the accepted physical data evidence |
| HW-13 | Complete as `ws004-p019` in `q049` | Independent standards-based CDC ECM driver and QEMU `usb-net` common-path baseline; no speculative shared ECM/NCM backend | HW-01, HW-12 foundations, NET-00 | QEMU selects ECM, publishes `ue0`, passes static/DHCP/ping in IDE and concurrent USB-storage topologies, honors zero-packet transfers, and preserves detach/reconnect ownership |
| HW-20 | Deferred built-in follow-up; existing ID retained | RTL8822CE (`10ec:c822`, subsystem `10ec:c130`) architecture and native PCIe driver | BR-00, HW-00, stable generic WLAN core, separate 8822C firmware decision | Scan, authenticate, associate, and exchange data on the exact built-in hardware in a later Phase |
| HW-21 | Deferred built-in follow-up; existing ID retained | Testable RTL8822CE-specific PCI/firmware hardware abstraction or protocol fixture | HW-20 design and the completed generic common-core fixture | Driver-specific state/error paths pass without claiming QEMU emulates the laptop radio |
| HW-22 | Complete as `ws004-p026` (`q055`; q040 intake retained) | Exact Japan-market T3U Nano identity and optional Realtek firmware package/license policy | Physical adapter, primary-source record | HW-T32 retains the full authoritative descriptor, explicit absence of printed revision, pinned upstream bytes/license, immutable GitHub acquisition mirror, separate install path, and update rule |
| HW-23 | Complete as `ws004-p027` (`q055`) | Generic WLAN ioctl UAPI, persistent kernel station core, scan cache/state/lifetime, and deterministic fake device | HW-22 documentary capability boundary, p012 | HW-T30 passes ABI, state, race, detach, and secret-erasure fixtures without a physical-radio claim |
| HW-24 | P036 complete (`q056`); p028 automatic milestone complete (`q057`) | Exact RTL8822BU USB/pre-radio substrate, separately selected firmware, BSD-licensed tables, and conservative 2.4-GHz/20-MHz scan | HW-22, HW-23, p010/p011/p015 | P036 package/transport/parser/lifetime and HW-T31 table/radio/scan automatic gates pass; physical fields come from the single shared WS005 p008 ledger |
| HW-25 | Automatic milestone complete as `ws004-p029` (`q058`) | WPA2-Personal/CCMP authentication, association, key installation, controlled port, and Ethernet L2 | HW-24, kernel entropy and reviewed crypto substrate | HW-T33 passes automatic handshake/replay/CCMP/negative fixtures; the eventual secure-L2 fields come from the same p008 ledger with no p029-specific request |
| HW-26 | Planned/deferred as `ws004-p030`; starts after WS005 p009 | Rekey, bounded reconnect, reset, hotplug, shutdown, and final exact-hardware hardening | HW-25, WS005 p009, controlled AP, WS005 p008 | HW-T34 passes automatic fault/race/storage gates and references the one shared p008 lifecycle checkpoint/five-run frozen-artifact ledger |
| HW-27 | Complete as `ws004-p031` (`q047`) | Concurrent UHCI/EHCI per-endpoint scheduling, request-local retirement, and runtime root-port lifecycle | p009--p011, p015, p016 | HW-T25 ordinary/sanitizer/analyzer and configured production gates, shared-INTx and USB regressions, repository build, plus standalone UHCI and paired EHCI/UHCI QEMU cells pass |
| HW-28 | Complete as `ws004-p032` (`q047`) | Ordered USB endpoint-halt recovery and conservative direct-root device reset shared by xHCI/UHCI/EHCI | HW-27, p010/p011/p015/p016 | HW-T26, reclaim-safe reserve gates, xHCI and paired UHCI/EHCI QEMU controls pass; no physical-machine recovery result is claimed |
| HW-29 | Complete as `ws004-p033` (`q047`) | Early-safe amd64 PC/AT framebuffer-console serialization and strict cell/pixel bounds | q047 p031 stress observation, existing console contract | HW-T27 host/sanitizer/input/build gates and forced `q047-legacy-hcd-final4` standalone/paired QEMU cells pass without console fault, corruption, or stall |
| HW-30 | Proposed | i915 hardware foundations for the discovered 11th-generation GPU | BR-00, HW-00, GFX UAPI | Modeset/scanout and recovery on hardware; model tests for device-independent layers |
| HW-31 | Planned/deferred as `ws004-p034`; nonblocking; not queued | CDC ECM adoption of q054 asynchronous TX-error accounting | p019, p017/q054 | HW-T28 proves exactly-once genuine terminal errors, excluded administrative cancellation, retained packet/byte/drop meanings, lifecycle safety, and unchanged QEMU ECM behavior without a physical check |
| HW-32 | Planned/deferred as `ws004-p035`; nonblocking; not queued | Bounded same-endpoint multi-URB support for xHCI | A measured consumer need; existing p011 per-endpoint contract | HW-T35 proves exact per-request completion/cancel/drain, ring wrap, teardown, and fairness while UHCI/EHCI retain one request per endpoint |
| HW-33 | Planned/deferred; not queued | Native Intel Wi-Fi 6 AX201 driver with its own `userland/firmware/intelax201/` entry | Exact PCI/subsystem inventory, HW-00, completed generic WLAN core, Intel firmware/license review | A future Phase freezes the device/firmware identity before scan, association, and data-path work; no RTL88 implementation is reused |

## 4. NVMe sequence

1. `ws004-p022` implements bounded controller reset, one admin queue, Identify,
   one 512-byte namespace, one message interrupt, and `/dev/nvme0n1` naming.
2. `ws004-p023` adds one I/O queue, bounce-buffer PRPs, read/write/flush,
   timeout/reset, shutdown, and detach without pretending that the current DMA
   layer supplies scatter/gather or IOMMU isolation.
3. `ws004-p024` runs integrity, concurrency, flush, reset, power-cycle, GPT
   partition, IDE, and USB-root gates using disposable QEMU images.
4. `ws004-p025` identifies and reads the Latitude SN740 (`15b7:5015`) without
   modifying it.
5. Physical file writes occur only later through WS019's explicit no-format
   installer contract and WS003 p018 acceptance; GPT/mkfs/native writes remain
   still later work.

File-backed overlay root on NVMe is the first end-to-end installer acceptance
after the driver is stable. Native UFS root follows separately; neither is
part of the USB-root milestone or the read-only hardware checkpoint.

## 5. WLAN sequence

The first WLAN target is now the USB TP-Link Archer T3U Nano, ahead of the
Latitude's built-in PCI WLAN. The FCC V1.0 record supplies documentary
RTL8822BU family evidence and the software USB identity is TP-Link
`2357:012e`; the earlier RTL8828BU guess is rejected. The purchased Japan-market
unit is labelled only `Archer T3U Nano` and has no printed revision. Its retained
`2357:012e`, `bcdDevice=2.10`, `ff/ff/ff`, five-endpoint descriptor is the
binding authority for this exact unit. TP-Link labels such as V1.40, V1.46,
V1.60, and V1.80 remain inference-only for other units: a shared download
archive does not prove an unchanged chip, USB ID, endpoint layout, or RF front
end.

The ordered implementation path is:

1. [`ws004-p026`](phase026-archer-t3u-nano-identity-firmware/phase.md) records
   the Japan-market label and absent printed revision, makes the complete USB
   descriptor authoritative for this unit, pins one 8822B firmware blob, and
   freezes the explicit optional-package/license/update policy.
2. [`ws004-p027`](phase027-wlan-uapi-common-core/phase.md) adds the generic
   versioned ioctl ABI and long-lived common kernel station state, then proves
   scan/cache/state/error/detach behavior with a deterministic fake device.
3. [`ws004-p028`](phase028-rtl8822bu-usb-scan/phase.md) adds only the
   RTL8822BU USB, efuse/RF, firmware, descriptor, and key-CAM hooks needed to
   publish `wlanN` and scan a conservative 2.4-GHz/20-MHz profile.
4. [`ws004-p029`](phase029-wpa2-ccmp-l2/phase.md) completes strict
   WPA2-Personal/CCMP authentication, association, four-way handshake,
   controlled-port authorization, and Ethernet L2 without DHCP.
5. [`ws005-p009`](../ws005-networking/phase009-wlan-minimum-connectivity/phase.md)
   first proves one direct physical carrier/DHCP/ping/fetch path without
   rekey, recovery, or repeatability expansion.
6. [`ws004-p030`](phase030-wlan-lifecycle-hardware-hardening/phase.md) then completes
   rekey, bounded reconnect, reset, up/down, unplug/reinsert, shutdown,
   concurrent-storage regression, and exact-hardware reliability using the
   same physical checkpoint and final five-run ledger as WS005 p008.

P026 and p027 are complete through q055, q056 completed the extracted p036
pre-radio substrate, and q057 completed p028 with the resolved BSD-3-Clause
table policy. Q058 completed p029; p030 remains an M/W/P planning entry. P028 uses one
persistent bulk-IN URB; the deferred p035 same-endpoint ring is a throughput
extension and is not in this dependency chain.

### Responsibility boundary

| Owner | Responsibilities |
| --- | --- |
| Common kernel WLAN layer | Versioned ioctl/status, scan generation/cache and BSS choice, 802.11 authentication/association, WPA2 state and crypto, rekey/reconnect, controlled port, Ethernet/802.11 conversion, carrier, cancellation, and secret lifetime |
| RTL8822BU chip/USB driver | Exact descriptor binding, USB control/bulk transport, efuse/RFE/radio/channel, firmware upload/events, hardware TX/RX descriptors and status, key CAM and CCMP offload, reset/quiesce |
| WS005 control plane | User-facing `wifi` and `net wifi` commands, per-user/root credential-file policy, automatic profile selection, and starting `dhcpc` only after L2 authorization |

The kernel common layer is deliberately long-lived: a one-shot `wifi connect`
process cannot own EAPOL retransmission, GTK rekey, link-loss handling, or
controlled-port state after it exits. Conversely, the chip driver does not
parse a passphrase, choose an SSID, run WPA, or raise carrier. DHCP begins only
after p029's authorized L2 result and remains outside WS004.

The first usable radio milestone is station-mode 2.4 GHz, non-DFS, 20 MHz,
WPA2-Personal/CCMP. 5 GHz, DFS/radar, HT/VHT optimization, aggregation,
power-save tuning, WPA3/SAE, 802.1X, AP/monitor mode, and roaming are later
capabilities, not implicit AC1300 completion criteria.

QEMU provides no faithful 802.11 RF model. Host fixtures and a constrained fake
device prove the generic state/lifetime logic only; USB transport fixtures
prove ownership only. Scan, association, encrypted L2, rekey, and final
reliability require the descriptor-confirmed physical adapter and a controlled
AP. WS005 p009 first records one deliberately narrow developmental
scan/connect/DHCP/ping/fetch observation after p029. That observation is not a
p028--p030 closure gate and does not consume final acceptance. After all
p028--p030 and control-plane automatic gates, WS005 p008 owns the combined
lifecycle checkpoint and the later frozen-artifact
five-consecutive-run ledger, which p028--p030 reference for physical completion.

### Firmware and retained built-in target

RTL8822BU uses `rtw88/rtw8822b_fw.bin`. The approved p036 firmware-entry
contract requires `userland/firmware/rtl8822b/` to
acquire the blob and `LICENCE.rtlwifi_firmware.txt` only from the immutable
`https://github.com/endlessm/linux-firmware.git` mirror revision
`2f56219d20e4becccd718963fc3bcc671c543ce5`, verify the frozen size and SHA-256
values, and stage them for separate package installation. Official
upstream provenance remains `linux-firmware` commit
`458e40fdbb4dad5134ec230a42df21aea1b5baf8` with its retained WHENCE and license
records. The base source/image contains no binary, an ordinary build performs
no firmware fetch, and the kernel performs no runtime network download.
Missing, wrong-digest, incompatible, or unapproved newer firmware fails visibly
with carrier down.

The built-in WLAN identity remains Realtek RTL8822CE, PCI `10ec:c822`,
subsystem `10ec:c130`. It is a later HW-20/HW-21 path, not an alias for the USB
device: it uses the 8822C family, PCIe transport, and
`rtw88/rtw8822c_fw.bin`. The generic common core is intended to be reused, but
no p026--p030 completion claims the built-in device.

Do not introduce a common RTL88 chip module before both implementations prove
a truly identical boundary. A future RTL8822CE Phase owns
`userland/firmware/rtl8822c/`; a later Intel AX201 Phase owns
`userland/firmware/intelax201/`. The Intel target is recorded from the Linux
debug host and remains deferred behind the Archer-first sequence.

On FreeBSD the built-in PCI WLAN inventory is collected with:

```sh
pciconf -lv | grep -A1 -B3 network
```

Retain the full matching stanza, including `vendor`, `device`, `subvendor`, and
`subdevice`. The supplied stanza is now the canonical RTL8822CE target identity.

References:

- FCC Equipment Authorization System V1.0 internal-photo record for FCC ID
  `2AXJ4T3UNANO`:
  <https://apps.fcc.gov/eas/GetApplicationAttachment.html?id=5468516>
- TP-Link official driver archive containing the 8822B/`2357:012e` mapping:
  <https://static.tp-link.com/upload/driver/2025/202512/20251231/Archer%20T3U%20Nano.zip>
- Linux mainline maps `2357:012e` to `rtw8822b_hw_spec` and names
  `rtw88/rtw8822b_fw.bin`:
  <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/realtek/rtw88/rtw8822bu.c>
  and
  <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/realtek/rtw88/rtw8822b.c>
- FreeBSD network-adapter inventory procedure:
  <https://docs.freebsd.org/en/books/handbook/network/>
- FreeBSD `usbconfig(8)` descriptor inspection:
  <https://man.freebsd.org/cgi/man.cgi?query=usbconfig&sektion=8>
- The exact Realtek binary-firmware terms are separate from the driver source
  license:
  <https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/LICENCE.rtlwifi_firmware.txt>
- FreeBSD distributes rtw88 firmware as a separate package rather than
  treating it as driver source:
  <https://cgit.freebsd.org/ports/tree/net/wifi-firmware-rtw88-kmod>

## 6. Driver completion rule

A driver is complete only when the common lifecycle is covered: discovery,
normal operation, concurrency, timeout, reset, detach/shutdown, and diagnostic
reporting. A probe-only implementation or a command that returns success while
using a stub is partial, not complete.

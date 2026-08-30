# WS004: hardware expansion

Last updated: 2026-08-31

WSID: `ws004`

Status: audited active; WS is not complete. q041 completed p016 checked
legacy-HCD request retirement and is proceeding to its independent WS005 and
WS008 items. `ws004-p026` completed every
automatic/read-only intake field in q040 and is uncleared only at the printed
unit label. `ws004-p020` deterministic CDC NCM hardening and
`ws004-p022`--`p024` NVMe discovery/I/O/strict-GPT QEMU acceptance are
complete; `ws004-p021` remains ready but independent; `ws004-p027`--`p030`
define the later Archer T3U Nano WLAN implementation path

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
`p021` records the independent native-xHCI Max ESIT defect exposed by the same
audit and remains outside q029. `p016` now completes the controller-proven
legacy-HCD hardware-retirement follow-up exposed by that audit. `p018`
corrected the IAD-less, Union-associated NCM match exposed by
the first physical RTL8156 insertion, and the accepted follow-up published
`ue0`. The broader asynchronous-TX/accounting decisions remain in p017.

Resume point: the 2026-08-30 completion audit found p016, p017, p021, p025,
and p026--p030 still open. q030 completed p022 through p024, including strict
primary/backup GPT and the final disposable QEMU acceptance matrix. The later
p025 is the single read-only Latitude SN740 checkpoint. Complete the remaining
automatic loader/installer prerequisites before requesting that physical
checkpoint. p021 remains a ready
but independent xHCI specification cleanup. p019 remains an independent ECM
baseline. p016 and the remaining asynchronous-TX/accounting portion of p017
stay later WS004 work. WLAN planning has resumed with the Archer T3U Nano as
the first target. `p026` must confirm the physical adapter descriptor and
separate firmware-package policy; `p027`--`p030` then progress through the
generic kernel WLAN core, RTL8822BU USB scan, WPA2-Personal/CCMP L2, and final
lifecycle hardening. These five Phases are planning entries only and have not
crossed the Queue boundary.

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
| [`ws004-p017`](phase017-cdc-ncm-runtime-recovery/phase.md) | Pending; not queued; residual TX-accounting policy open | p020 extracts the approved valid-sequence/resync and bounded-work rules; p017 retains asynchronous terminal TX accounting and any later separately approved recovery work |
| [`ws004-p018`](phase018-rtl8156-ncm-association/phase.md) | Complete (`q028`) | CDC Union is authoritative and IAD is optional strict corroboration; automatic gates pass and physical RTL8156 configuration 2 binds and publishes `ue0`; carrier/data work moved to WS005 p001 |
| [`ws004-p019`](phase019-cdc-ecm-qemu-baseline/phase.md) | Deferred; trigger no longer present | Real RTL8156 NCM carrier/DHCP/ping passed through QEMU and on Latitude, so the diagnostic ECM fallback is not required by the current WLAN/NVMe path; retain it only as an independently selected future class-driver baseline |
| [`ws004-p020`](phase020-cdc-ncm-deterministic-hardening/phase.md) | Complete (`q029` automatic software scope) | Valid sequences accept/resynchronize, malformed input preserves state, completions and rearms are bounded/fair, and the packet filter is programmed transactionally on open; focused and regression gates pass |
| [`ws004-p021`](phase021-xhci-superspeed-interrupt-context/phase.md) | Planned; ready for Queue proposal; not queued | Encode legal SuperSpeed interrupt companion `wBytesPerInterval` as xHCI Max ESIT/Average TRB Length, reject malformed descriptors, and preserve every other endpoint context |
| [`ws004-p022`](phase022-nvme-admin-identify/phase.md) | Complete (`q030`) | Bounded reset/admin Identify, transactional PCI/MSI lifecycle, stable names, focused fixtures, amd64/i386 builds, exact non-mutating QEMU namespace, IDE, and USB-root gates pass |
| [`ws004-p023`](phase023-nvme-io-lifecycle/phase.md) | Complete (`q030`) | One depth-64 I/O queue, private 4-KiB bounce slots, checked 64-bit read/write, truthful flush, concurrent wrap, timeout/reset, normal shutdown, and quarantine pass focused/build/QEMU/regression gates |
| [`ws004-p024`](phase024-nvme-qemu-acceptance/phase.md) | Complete (`q030`) | Strict 512/4096 GPT host gates and disposable QEMU partition write/flush/restart/rejection plus IDE, xHCI USB-root, amd64, and i386 gates pass |
| [`ws004-p025`](phase025-latitude-nvme-readonly/phase.md) | Planned physical checkpoint; depends on p024 | Latitude SN740 `15b7:5015` identifies and reads safely without modifying internal storage |
| [`ws004-p026`](phase026-archer-t3u-nano-identity-firmware/phase.md) | Uncleared (`q040`) | Complete descriptor/ID/firmware/license/package evidence retained; supply only the purchased unit's printed model/region/revision to reconcile and clear it |
| [`ws004-p027`](phase027-wlan-uapi-common-core/phase.md) | Planned; not queued | Add the versioned pointer-free WLAN ioctl ABI, persistent common station state, generation-safe scan/status/cache/lifetime, and a deterministic fake radio without claiming hardware |
| [`ws004-p028`](phase028-rtl8822bu-usb-scan/phase.md) | Planned; depends on p026/p027; not queued | Bind only the descriptor-confirmed RTL8822BU interface, load the pinned optional firmware, and implement conservative 2.4-GHz/20-MHz scan; physical attach/scan evidence is one field of the shared WS005 p008 ledger, not a p028 run |
| [`ws004-p029`](phase029-wpa2-ccmp-l2/phase.md) | Planned; depends on p028 automatic milestone; not queued | Common-kernel WPA2-Personal/CCMP authentication, association, four-way handshake, key CAM, controlled port, and bidirectional Ethernet L2; physical evidence is shared with p030/WS005 p008 |
| [`ws004-p030`](phase030-wlan-lifecycle-hardware-hardening/phase.md) | Planned; depends on p029 automatic milestone; not queued | Rekey, bounded reconnect, reset, up/down, unplug/reinsert, shutdown, and concurrent-storage regressions; share one lifecycle checkpoint and the frozen-artifact five-run ledger with WS005 p008 rather than duplicate physical work |

### MSI follow-up register

| Item | Initial `ws004-p003` treatment | Resume condition |
| --- | --- | --- |
| Multi-message conventional MSI | Deferred; one registration and one message only | A consumer requires multiple power-of-two messages and has focused ordering tests |
| Dynamic MSI affinity | Deferred; mappings target CPU 0 | A public remap operation can return a replacement address/event pair safely |
| arm64 IORT/GIC ITS backend | Deferred; public signature preserved and port returns unsupported | An arm64 PCIe platform Phase supplies firmware and interrupt-controller fixtures |
| Non-PCI message source prefixes | Deferred; only canonical PCI BDF is accepted | A concrete platform device needs message interrupts and defines stable source identity |

q029, p020, and WS005 p001 are complete through final Latitude-native external
fetch. p021 remains ready for a future Queue as an independent standards
correction, not an active failure response. Later WS004 candidates are
p016/p017, p019, p021, p025, and the planned-only p026--p030 WLAN chain, plus
HW-11, HW-20/HW-21, and HW-30 when their inputs and acceptance environments
are available. q040 selects the evidence/policy-only p026 boundary; it does
not authorize p027--p030 implementation.

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
| HW-12 | Active (`q029` p020 hardening; q028 bind complete) | Independent CDC NCM class driver on the common USB/xHCI/net-device lifetime foundations; Union-associated NCM does not require an IAD; deterministic valid-sequence recovery, bounded completion work, and post-alternate open filtering precede one physical data check | HW-01 | p020 automatic gates pass, then WS005 proves or precisely bounds carrier/static/DHCP traffic on the Latitude RTL8156 |
| HW-13 | `ws004-p019` planned fallback | Independent standards-based CDC ECM driver and QEMU `usb-net` common-path baseline; no speculative shared ECM/NCM backend | HW-01, HW-12 foundations, NET-00, failed q029 physical discriminator | If needed after q029, QEMU selects ECM, publishes `ue0`, passes static/DHCP/ping in IDE and concurrent USB-storage topologies, and preserves detach/reconnect ownership |
| HW-20 | Deferred built-in follow-up; existing ID retained | RTL8822CE (`10ec:c822`, subsystem `10ec:c130`) architecture and native PCIe driver | BR-00, HW-00, stable generic WLAN core, separate 8822C firmware decision | Scan, authenticate, associate, and exchange data on the exact built-in hardware in a later Phase |
| HW-21 | Deferred built-in follow-up; existing ID retained | Testable RTL8822CE-specific PCI/firmware hardware abstraction or protocol fixture | HW-20 design and the completed generic common-core fixture | Driver-specific state/error paths pass without claiming QEMU emulates the laptop radio |
| HW-22 | Planned as `ws004-p026`; not queued | Archer T3U Nano exact-unit identity and optional Realtek firmware package/license policy | Physical adapter, primary-source record | HW-T32 performs one read-only development-host descriptor inventory and freezes one blob revision, digest, license, path, and update rule before any bind; missing inventory leaves p026 uncleared |
| HW-23 | Planned as `ws004-p027`; not queued | Generic WLAN ioctl UAPI, persistent kernel station core, scan cache/state/lifetime, and deterministic fake device | HW-22 documentary capability boundary, p012 | HW-T30 passes ABI, state, race, detach, and secret-erasure fixtures without a physical-radio claim |
| HW-24 | Planned as `ws004-p028`; not queued | Exact RTL8822BU USB attach, separately packaged firmware start, and conservative 2.4-GHz/20-MHz scan | HW-22, HW-23, p010/p011/p015 | HW-T31 passes automatic attach/firmware/scan gates; the eventual physical fields come from the single shared WS005 p008 ledger with no p028-specific request |
| HW-25 | Planned as `ws004-p029`; not queued | WPA2-Personal/CCMP authentication, association, key installation, controlled port, and Ethernet L2 | HW-24, kernel entropy and reviewed crypto substrate | HW-T33 passes automatic handshake/replay/CCMP/negative fixtures; the eventual secure-L2 fields come from the same p008 ledger with no p029-specific request |
| HW-26 | Planned as `ws004-p030`; not queued | Rekey, bounded reconnect, reset, hotplug, shutdown, and final exact-hardware hardening | HW-25, controlled AP, WS005 p008 | HW-T34 passes automatic fault/race/storage gates and references the one shared p008 lifecycle checkpoint/five-run frozen-artifact ledger |
| HW-30 | Proposed | i915 hardware foundations for the discovered 11th-generation GPU | BR-00, HW-00, GFX UAPI | Modeset/scanout and recovery on hardware; model tests for device-independent layers |

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
Latitude's built-in PCI WLAN. The verified V1.0 documentary identity is
RTL8822BU and the software USB identity is TP-Link `2357:012e`; the earlier
RTL8828BU guess is rejected. The purchased unit must still supply its own exact
descriptor before binding. TP-Link labels such as V1.40, V1.46, V1.60, and
V1.80 remain inference-only until independently inspected: a shared download
archive does not prove an unchanged chip, USB ID, endpoint layout, or RF front
end.

The ordered implementation path is:

1. [`ws004-p026`](phase026-archer-t3u-nano-identity-firmware/phase.md) captures
   the unit label and complete USB descriptor, pins one 8822B firmware blob,
   and freezes the optional-package/license/update policy.
2. [`ws004-p027`](phase027-wlan-uapi-common-core/phase.md) adds the generic
   versioned ioctl ABI and long-lived common kernel station state, then proves
   scan/cache/state/error/detach behavior with a deterministic fake device.
3. [`ws004-p028`](phase028-rtl8822bu-usb-scan/phase.md) adds only the
   RTL8822BU USB, efuse/RF, firmware, descriptor, and key-CAM hooks needed to
   publish `wlanN` and scan a conservative 2.4-GHz/20-MHz profile.
4. [`ws004-p029`](phase029-wpa2-ccmp-l2/phase.md) completes strict
   WPA2-Personal/CCMP authentication, association, four-way handshake,
   controlled-port authorization, and Ethernet L2 without DHCP.
5. [`ws004-p030`](phase030-wlan-lifecycle-hardware-hardening/phase.md) completes
   rekey, bounded reconnect, reset, up/down, unplug/reinsert, shutdown,
   concurrent-storage regression, and exact-hardware reliability using the
   same physical checkpoint and final five-run ledger as WS005 p008.

All five are M/W/P planning entries. Each still requires a finite Queue
proposal and explicit execution approval; this sequence is not itself an
implementation Queue.

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
AP. Their first zedBSD observation is one combined WS005 p008 checkpoint after
all p028--p030 and control-plane automatic gates; p028 and p029 make no
independent physical request. p008 alone owns the later frozen-artifact
five-consecutive-run ledger, which p028--p030 reference for physical completion.

### Firmware and retained built-in target

RTL8822BU uses `rtw88/rtw8822b_fw.bin`. It is installed only by a separately
identified optional firmware package containing the exact unmodified upstream
blob, `LICENCE.rtlwifi_firmware.txt`, provenance, revision, size, SHA-256, and
update policy. The base source/image does not contain the binary and performs
no build-time or runtime download. Missing, wrong-digest, incompatible, or
unapproved newer firmware fails visibly with carrier down.

The built-in WLAN identity remains Realtek RTL8822CE, PCI `10ec:c822`,
subsystem `10ec:c130`. It is a later HW-20/HW-21 path, not an alias for the USB
device: it uses the 8822C family, PCIe transport, and
`rtw88/rtw8822c_fw.bin`. The generic common core is intended to be reused, but
no p026--p030 completion claims the built-in device.

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

# WS004 shared test cases

Parent: [WS004](../ws.md)

| Case ID | Area | Required observation |
| --- | --- | --- |
| HW-T00 | PCIe/DMA/interrupts | BARs, capability walking, DMA widths/order, shared INTx fanout/checked teardown, MSI/MSI-X setup/teardown, and timeout cleanup pass focused tests |
| HW-T01 | ECAM/MSI HAL contract | Canonical source parsing, MCFG validation, vector allocation/exhaustion/reuse, PCI register images, rollback, in-flight unregister, and real QEMU delivery pass |
| HW-T02 | Legacy PCI HCD IRQ teardown | EHCI and UHCI retain the IRQ cookie/allocation, DMA, BAR-or-I/O ownership, HCD bus, handler argument, and controller after checked removal failure; retry releases each resource exactly once |
| HW-T03 | Legacy HCD request retirement | UHCI requires request-local unlink plus an observed FRNUM boundary, EHCI requires schedule-local unlink plus its checked periodic barrier or fresh matching Async Advance acknowledgement, and both retain request/URB/DMA ownership on every checked failure; p016's former single-request/all-frame shape is historical |
| HW-T10 | xHCI model | QEMU enumeration, control/bulk/interrupt transfers, reconnect, timeout, and controller reset pass |
| HW-T11 | USB storage | Root-continuity cases from WS003 pass through xHCI |
| HW-T12 | USB overlay writes | Correlated URB/heap tests pass; 500 sequential q35/xHCI/SMP=4 boots from pristine raw-image copies have zero kernel/storage-error markers; explicit `DATA.IMG` persistence and IDE control pass; detailed manual acceptance follows |
| HW-T13 | PC/AT warm reset | Three consecutive guest reboots reach fresh login with clean kernel BSS state |
| HW-T14 | USB function model | Multiple configurations, IAD/Union, class-specific descriptors, alternate endpoints, strings, sibling claims, and failed-selection rollback pass production-source fixtures |
| HW-T15 | Concurrent xHCI URBs | Control, interrupt, bulk RX/TX, and independent storage requests complete out of order; isolated cancel, quiesce, and reclaim reserve invariants pass |
| HW-T16 | Removable net device | Carrier changes, queued-RX detach, ARP/route purge, locked asynchronous TX-error accounting during removal, deferred release, shutdown, and more than eight reconnects pass without stale ownership |
| HW-T17 | CDC NCM wire and driver | NTH16/NDP16 valid/malformed fixtures, strict negotiation, bind/unwind, RX/TX, terminal-result accounting, administrative cancellation, notification, detach, reconnect, and concurrent storage pass |
| HW-T18 | USB binding transactions | Idle inactive-alt URBs, interface-scoped switch admission, sibling concurrency, submit races, provisional attach abort, detach retention, EP0 serialization, and Mass Storage regression pass |
| HW-T19 | RTL8156 NCM association | An unsupported-vendor/NCM/ECM three-configuration fixture selects Union-associated NCM without requiring an IAD; matching IAD corroborates, contradictions reject, binding diagnostics identify selection/outcome, and one final Latitude insertion publishes `ue0` |
| HW-T20 | NVMe QEMU | Admin/I/O queue ownership, Identify, namespace bounds, read/write/flush, concurrency, reset, strict primary/backup GPT discovery, and failure tests pass on disposable images |
| HW-T21 | NVMe hardware | The Latitude SN740 identifies and completes bounded reads without modification before any separately confirmed installation write; exact device and error logs are stored |
| HW-T22 | CDC ECM QEMU baseline | A QEMU RNDIS-first/ECM-second `usb-net` function selects ECM without a quirk, publishes `ue0`, carries raw Ethernet and DHCP/ping traffic, detaches cleanly, and coexists with xHCI USB Storage |
| HW-T23 | CDC NCM deterministic hardening | Arbitrary first and mismatched fully valid sequences deliver and resynchronize, malformed NTBs preserve state, zero-delivery completions consume bounded poll work, and each open programs the packet filter after the active alternate and before input URBs |
| HW-T24 | xHCI SuperSpeed interrupt context | Companion `wBytesPerInterval` validation and host-endian decode produce exact Max ESIT/Average TRB Length fields for RTL8156 EP3; zero, oversized, reserved-field, 3072-byte ceiling, and 3073 rejection cases behave as specified while non-SS/non-interrupt contexts remain unchanged |
| HW-T25 | Legacy HCD concurrency and hotplug | Shared PCI INTx dispatch lets all paired controllers attach; UHCI/EHCI accept independent endpoint owners, retire only target schedule graphs, keep interrupt and Storage progress concurrent, and perform detach/reinsert only through independent root workers |
| HW-T26 | Checked USB recovery | Core STALL latching, ordered endpoint clear-halt, xHCI ring recovery, UHCI/EHCI DATA0, direct-root reset, and Mass Storage migration preserve exact callback/URB/DMA ownership under failure |
| HW-T27 | amd64 framebuffer console serialization | Concurrent CPU/IRQ-style output, terminal same-CPU re-entry, invalid cell coordinates, and framebuffer extent guards preserve cursor/cell/pixel state; the final HW-T25 QEMU run has no console fault or stall |
| HW-T30 | Generic WLAN logic | The versioned pointer-free ioctl ABI, scan generations/cache, station state, cancellation, carrier, detach, race, and secret-erasure rules pass against a deterministic fake radio without claiming RF success |
| HW-T31 | RTL8822BU attach/scan | Only the descriptor-confirmed `2357:012e` interface binds; automatic firmware/USB/scan gates pass, and the physical attach/scan fields come from the one shared WS005 p008 ledger with no p028-specific run |
| HW-T32 | Archer identity/firmware policy | The exact adapter label and complete descriptor are retained; V1.0 RTL8822BU evidence is separated from later-revision inference, and one separately packaged firmware blob/license/provenance/digest/update rule is frozen before binding |
| HW-T33 | WPA2-Personal/CCMP L2 | Crypto vectors and strict positive/negative RSN/authentication/association/four-way/replay/key-CAM/controlled-port fixtures pass; physical secure-L2 fields come from the one shared WS005 p008 ledger with no p029-specific run |
| HW-T34 | WLAN lifecycle hardening | Rekey, bounded reconnect, firmware/USB recovery, up/down, unplug/reinsert, shutdown, concurrent storage, and race/fault fixtures pass; one shared WS005 p008 lifecycle checkpoint and its frozen-artifact five-run batch supply nonduplicated physical evidence |
| HW-T40 | i915 foundations | Device-independent UAPI/model tests pass; modeset/scanout/reset require target-hardware evidence |

QEMU/model and physical-hardware results are always separate evidence fields.

## HW-T25 legacy HCD concurrency and hotplug

[`ws004-p031`](../phase031-legacy-hcd-concurrent-hotplug/phase.md) owns a
production-source/model runner in ordinary, ASan/UBSan, and analyzer modes,
then standalone UHCI and paired EHCI/UHCI QEMU cells.  The runtime cells log in
through PS/2 using QMP `input-send-event` commands with explicit key-down and
key-up pairs, then hot-add a full-speed `usb-mouse` as the bounded HID probe
alongside independent USB Storage.  QMP-wrapped `mouse_move`, `device_del`, and
`device_add` commands exercise 11 detach/reinsert transitions across 12 HID
generations; generation 12 is re-armed and left pending across normal reboot.
The cells require clean worker/DMA retirement and do not claim production HID
or evdev behavior.

The paired q35 topology routes its EHCI and three UHCI controllers over two
shared legacy INTx lines.  The focused fixture verifies one HAL registration
and one EOI per line, delivery to every PCI subscriber, non-final removal
without masking a peer, in-flight `EBUSY` retention, and final checked-removal
failure followed by retry:

```sh
mkdir -p build/q047-tmp
TMPDIR="$PWD/build/q047-tmp" \
  cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  src/drivers/pci.c \
  plan/ws004-hardware/tests/pci-shared-intx-test.c \
  -o build/q047-tmp/pci-shared-intx-test
build/q047-tmp/pci-shared-intx-test
```

Scope note: this fixture covers dispatcher/list/drain lifetime, not a live
function which remains asserted after unsubscribe.  EHCI and UHCI mask their
controller-local source before checked PCI removal.  Generic PCI support for
setting and restoring `PCI_COMMAND.INTx Disable` remains a known follow-up and
is not claimed by HW-T25.

The forced-canonical `build/q047-legacy-hcd-final4` QEMU result passes both
cells under QEMU 10.0.11.  Each cell records 12 attaches/detaches, 11
detach/reinsert transitions, four accepted and completed Storage requests,
payload comparison, final pending re-arm across reboot, checked worker joins,
and unchanged input hashes.  The paired cell records three UHCI plus one EHCI
controller, 12 Mb/s companion-UHCI mouse ownership, and 480 Mb/s EHCI Storage;
it does not claim EHCI high-speed periodic runtime coverage.  Together with
the passing ordinary/sanitizer/analyzer, configured-production, focused
regression, shared-INTx, and repository-build gates, this completes HW-T25 and
`ws004-p031`.

## HW-T26 checked USB recovery

[`ws004-p032`](../phase032-usb-endpoint-device-recovery/phase.md) owns the
production USB-core/HCD recovery fixture.  The fake HCD drives the production
`usb.c` implementation directly and checks latch-before-callback visibility,
callback re-entry, exact `CLEAR_FEATURE(ENDPOINT_HALT)` wire/HCD/unlatch
ordering, preventive clears, endpoint-owner and active-URB admission, and every
accepted-wire ambiguity or HCD failure quarantine.  Device-reset cases cover
direct-root and disabled-port preflight with zero side effects, active and
multiple binding owners, retained alternates and sibling claims, exact
disable/quiesce/port/device/address/configuration/alternate/endpoint-reset
ordering, rollback before the destructive boundary, quarantine afterward, and
stable binding/generation/recovery-URB lifetime.

The same runner keeps existing fake HCDs compatible with the mandatory
`endpoint_reset` operation and rejects registration without it.  Production
source gates require xHCI retained-ring and Set-TR-Dequeue restart behavior,
CSC-preserving root reset, UHCI/EHCI DATA0 plus completion-inflight lifetime,
and common-API-only Mass Storage recovery.  It runs the focused fixture in
ordinary, ASan/UBSan, and GCC analyzer modes, the function/binding/unregister
regressions, and configured amd64/i386 production-object builds without
starting QEMU:

```sh
mkdir -p build/q047-tmp
TMPDIR="$PWD/build/q047-tmp" \
  plan/ws004-hardware/tests/run-usb-recovery-contract-test.sh
```

QEMU normal controls remain distinct from the focused injected STALL/reset
evidence.

## HW-T27 amd64 framebuffer console serialization

[`ws004-p033`](../phase033-amd64-framebuffer-console-serialization/phase.md)
links the production amd64 PC/AT console into a four-writer host fixture.  It
checks framebuffer canaries, invalid coordinates, and same-CPU terminal
re-entry in ordinary and ASan/UBSan modes:

```sh
TMPDIR="$PWD/build/q047-tmp" \
  plan/ws004-hardware/tests/run-amd64-console-output-host-test.sh
```

The existing WS006 input ownership/resynchronization fixture remains the input
regression.  One fresh maintained HW-T25 QEMU matrix supplies the final runtime
evidence for both p031 and p033; HW-T27 does not request duplicate QEMU boots.
The forced-canonical `build/q047-legacy-hcd-final4` result passes standalone
UHCI and paired EHCI plus three UHCI companions for 11 detach/reinsert
transitions across 12 generations per cell, Storage comparison, and reboot
with no console fault, corruption, or stall.
Together with the ordinary and ASan/UBSan host runner and the WS006 input
regression, this completes HW-T27.

## HW-T24 xHCI SuperSpeed interrupt context

`ws004-p021` owns the pure strict context corpus, production USB descriptor
fixture, and pre-DMA source-order gate:

```sh
TMPDIR="$PWD/build/q045-tmp" \
  plan/ws004-hardware/tests/run-xhci-superspeed-interrupt-context-test.sh
```

Q052 revalidates ordinary and ASan/UBSan runs at 82 focused checks and 1,496
current production USB function checks; the xHCI model, compiler analyzer,
configured amd64/i386 production objects, and a fresh OVMF q35/xHCI USB-root
boot also pass. The final q052 audit replaces the incorrect 16-KiB interrupt
ceiling with the 3-KiB architectural limit and proves 3,072 acceptance plus
3,073 rejection with descriptor capacity still available. Exact RTL8156 words are
`0x000a0000`, `0x0010003e`, and `0x00100010`. The concurrent-URB, USB binding,
NCM wire/driver, USB-storage SCSI, and URB-publication regressions pass their
available ordinary, sanitizer, and analyzer gates. The q045 Noct blocker is
superseded. One q052 hash-pinned Latitude carrier/DHCP/ping/fetch observation
remains before p021 completion; no older image substitutes for it.

## HW-T30 generic WLAN logic

`ws004-p027` owns this production-source fixture. It uses a deterministic fake
radio and explicit test clock to verify:

- identical fixed-width UAPI layouts on amd64/i386, nonzero `_IOW/_IOR/_IOWR`
  encoded sizes and libc argument forwarding, version/size/reserved-field
  rejection, pointer-free one-record BSS pagination, and `ESTALE` snapshots;
- binary/hidden/32-octet SSIDs, 64-entry deterministic cache eviction,
  malformed/truncated information elements, duplicate BSS updates, and strict
  normalized security flags;
- scan start/repeated-start idempotent join/stop/idempotent-stop/timeout, exact
  generation publication, deterministic BSS selection, and rejection of late
  events from an old generation;
- all station-state, timeout, cancellation, driver-error, controlled-port, and
  carrier transitions without treating a fake association as hardware;
- concurrent list/status/connect/disconnect, the INET-to-`net_device` ioctl
  dispatch, active-ioctl admission/join, close and detach during every state,
  a callback blocked across teardown, and exact checked retry; and
- passphrase/key test patterns proven erased on failure, disconnect, close,
  detach, and shutdown, with no secret in UAPI or logs.

Ordinary, ASan/UBSan, compiler-analyzer, race-stress, configured amd64/i386,
`make -j16`, IDE, xHCI USB-root, wired-network, and net-device-hotplug gates are
required. An optional QEMU fake-device ioctl round trip remains model evidence
and is absent from ordinary images.

## HW-T31 RTL8822BU attach, firmware, and scan

`ws004-p028` first drives the production driver through a fake USB transport.
It covers exact and neighboring IDs/interface tuples/endpoints, every attach
allocation rollback, register width/endian/timeout handling, efuse cut/RFE/MAC/
channel-plan validation, firmware size/digest/header/page/ready/error cases,
descriptor bounds, concurrent URBs, scan stop, stale completions, retry, close,
and detach while unrelated USB storage remains active.

HW-T31 makes no independent physical request. After every p028--p030 and WS005
automatic gate passes, the one combined WS005 p008 provisional ledger supplies
its physical fields: exact `wlan0`, firmware/cut/RFE/endpoints, and bounded
controlled-AP scan. That same run continues to HW-T33, HW-T34, and DHCP/E2E;
p028 does not ask the user to repeat an attach or scan.

## HW-T32 Archer identity and firmware policy

The q040 read-only descriptor, independent 8822B mappings, pinned firmware and
license record, negative-input definitions, and exact remaining printed-label
checkpoint are retained in
[`archer-t3u-nano-intake.md`](archer-t3u-nano-intake.md).

`ws004-p026` retains the purchased adapter's model/region/hardware-revision
label and complete raw USB device/configuration/interface/alternate/endpoint
descriptor from one read-only `lsusb -v`/`usbconfig` inventory on an existing
development host. It is not a zedBSD boot or radio test. Serial number and
unrelated device identity are redacted. The positive binding input is
`2357:012e` plus the exact interface and endpoint tuple; product text,
vendor-only, broad Realtek, and neighboring IDs reject. If this inventory is
unavailable, p026 is `uncleared` and p028 binding remains ineligible.

The documentary evidence separately records:

- FCC `2AXJ4T3UNANO` V1.0 internal-photo marking `RTL8822BU`;
- the TP-Link INF 8822B mapping for `USB\VID_2357&PID_012E`;
- Linux mainline's `2357:012e` to `rtw8822b_hw_spec` mapping; and
- later TP-Link hardware labels as inference only until their own descriptor/
  physical evidence exists. RTL8828BU is not an accepted alias.

One official `linux-firmware` revision of `rtw88/rtw8822b_fw.bin` is pinned by
path, reported version, size, SHA-256, WHENCE entry, and exact
`LICENCE.rtlwifi_firmware.txt`. The optional package includes the unmodified
blob and license; the ordinary base image includes neither the blob nor a
download step. Absent, short, oversized, wrong-digest, and unsupported-header
negative inputs are retained for HW-T31.

## HW-T33 WPA2-Personal/CCMP L2

`ws004-p029` runs official SHA-1/HMAC/PBKDF2/AES/key-wrap known-answer vectors,
then scripted WPA2 traces through the production common core and fake driver.
It covers strict RSN CCMP+PSK selection, every authentication/association
field/status/timeout, exact EAPOL messages 1/4--4/4, nonce/MIC/replay/KDE
validation, atomic PTK/GTK CAM install, entropy and CAM failures, and carrier
only after controlled-port authorization. Retransmitted messages 1 and 3 must
not regenerate SNonce, reinstall a key, or reset a packet number.

Ethernet/LLC/SNAP and test-reference CCMP cases cover valid traffic plus wrong
BSSID/direction/key generation/key ID/PN/MIC/length, preauthorization filtering,
disconnect, and secret erasure. Wrong-passphrase, unsupported-security, and
malformed-handshake negatives remain automatic and never raise carrier or
transmit plaintext. HW-T33 makes no independent physical request: the one
combined WS005 p008 ledger supplies the controlled-AP association, authorized
carrier, and bounded bidirectional L2 fields before its DHCP/E2E stage, and the
same ledger also satisfies HW-T31/HW-T34.

## HW-T34 WLAN lifecycle hardening

`ws004-p030` extends the same production fixtures through group and pairwise
rekey, retransmission/replay/CAM failure, link loss at every state, firmware
stall/reload, endpoint timeout/stall/short/foreign completion, reconnect
backoff/cancellation/exhaustion, up/down, unplug/reinsert, and terminal shutdown.
At least 100 synthetic iterations run with concurrent xHCI USB-storage work and
prove exact timer/callback/URB/DMA/key/common/net-device retirement or checked
retention.

Physical evidence is not a second HW-T34 campaign. One shared WS005 p008
provisional script validates the exact adapter/artifacts, complete WLAN path,
one controlled link-loss/reconnect, down/cleanup, and one final removal while
USB storage remains responsive. A pass freezes the artifact; p008 alone owns
the final five-consecutive-cold-boot batch. The fifth run adds at most ten
minutes of bounded bidirectional traffic with concurrent storage. HW-T34
references that one redacted ledger, while forced rekey, repeated hotplug,
firmware/endpoint negatives, and 100-iteration stress remain automatic.

## HW-T20 NVMe I/O and lifecycle evidence

The p024 strict GPT fixture links the production `gpt.c`, PC/AT selector, and
legacy MBR parser.  Ordinary, ASan/UBSan, and compiler-analyzer runs cover
512- and 4096-byte logical blocks, both valid copies, either-copy degradation,
contradictory copies, header/table CRCs and geometry, authoritative hybrid
selection, no fallback after any GPT evidence, pure-MBR fallback, all-entry
validation before capacity failure, mixed-endian PARTUUID text, UTF-16LE and
UTF-8 boundaries, and preserved sparse GPT slot indexes:

```sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/run-gpt-host-test.sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/run-partition-publication-test.sh
```

The second production-source fixture continues from parsed metadata through
partition publication.  It proves unbounded decimal GPT slot naming such as
`nvme0n1p100`, the non-numeric-parent form `sda100`, and transactional cleanup
after allocation, name, or registry-creation failure.

The p024 QEMU harness constructs GPT sectors itself with the small host C tool;
it neither depends on a host partitioning utility nor modifies a production
image.  It extends the p023 raw-I/O runner through its explicit namespace
initializer and guest-device hooks.  Two boots write, flush, and verify
`/dev/nvme0n1p1`; a third boot attaches a separate namespace whose two GPT
header CRCs are damaged, requires strict rejection with no partition
publication, and still reaches login from the disposable IDE copy:

```sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/qemu-nvme-gpt.sh
```

The ordinary IDE and xHCI USB-root regressions remain separate controls so a
GPT failure cannot be hidden by changing the system-image transport.
The retained q030 host/QEMU observations are in
[q030 NVMe GPT evidence](q030-nvme-gpt-evidence.md).

The p023 host fixtures exercise command encoding/status translation, 64-bit
block-range arithmetic, and the private command/BIO/DMA ownership ledger under
normal completion, timeout/reset, quarantine, shutdown, detach, and
exactly-once release:

```sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/run-nvme-io-test.sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/run-nvme-io-lifecycle-test.sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/run-devfs-block-range-test.sh
```

The QEMU runner builds a separate WS004 test image containing
`/usr/bin/nvme-io-guest`; it never adds that helper to an ordinary image. The
guest opens the raw NVMe descriptor and performs `pwrite`, descriptor `fsync`,
`pread`, and comparison below and above 4 GiB. A second QEMU/controller boot
verifies both patterns from the same disposable namespace. Each boot also
executes 96 disjoint 4-KiB transfers and requires the QEMU device trace to show
both SQ1 and CQ1 wrapping to zero; the focused lifecycle fixture covers
multiple outstanding owners, out-of-order completion, and CID reuse. `dd`
followed by a shell-wide `sync` is deliberately not accepted as flush evidence:

```sh
mkdir -p build/q030-tmp
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/qemu-nvme-io.sh
```

## HW-00 host regressions

The foundation-audit regressions are ordinary host binaries and do not use a
repository-wide test target:

```sh
cc -std=c11 -Iinclude -Wall -Wextra -Werror \
  src/drivers/dma.c plan/ws004-hardware/tests/dma-constraints-test.c \
  -o /tmp/ws004-dma-test
/tmp/ws004-dma-test

cc -std=c11 -Iinclude -Wall -Wextra -Werror \
  src/drivers/pci.c plan/ws004-hardware/tests/pci-rescan-test.c \
  -o /tmp/ws004-pci-test
/tmp/ws004-pci-test

cc -std=c11 -Iinclude -Wall -Wextra -Werror \
  src/drivers/pci.c plan/ws004-hardware/tests/pcie-capability-test.c \
  -o /tmp/ws004-pcie-test
/tmp/ws004-pcie-test
```

## HW-T01 evidence

`ws004-p003` provides the following focused evidence:

- exact acceptance and rejection cases for `PCI SSSS:BB:DD.F`;
- valid, truncated, overlapping, overflowing, and bad-checksum MCFG fixtures;
- reserved-vector exclusion, exhaustion, reuse, and teardown under in-flight
  dispatch;
- conventional MSI and MSI-X register images, width rejection, masking order,
  and complete rollback; and
- real interrupt delivery and detach on a deterministic MSI-capable PCIe device
  under QEMU `q35`.

The source parser, MCFG parser, PCI rescan/capability, and PCI MSI fixtures are
compiled directly with `cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror` and
their matching source file. The QEMU fixture is linked only when
`CONFIG_KERNEL_TEST_CHECKPOINTS=y`; it boots `q35` with `-device edu` and checks
allocator exhaustion/reuse, a delivered MSI, and continued progress through
`login:`. It does not use `make check` or material from `.internal/`.

## HW-T02 checked legacy-HCD teardown

The lifecycle fixture applies the same ownership contract to EHCI and UHCI. It
distinguishes USB-core preflight `EBUSY` (quiesce is not entered and hardware
remains operational) from checked-IRQ `EBUSY` (hardware is halted but all
ownership remains). It also covers halt and bus-master-disable failures,
persistent checked-removal errors, stop/DMA release only after completed
quiesce, successful retry, and idempotent final detach without double-free:

```sh
cc -std=c11 -Wall -Wextra -Werror \
  plan/ws004-hardware/tests/pci-hcd-irq-teardown-test.c \
  -o /tmp/ws004-pci-hcd-irq-teardown-test
/tmp/ws004-pci-hcd-irq-teardown-test
```

## HW-T03 checked legacy-HCD request retirement

`ws004-p016` established the then-current single-active-request UHCI/EHCI
baseline and replaced software-only unlink with a controller-observed
retirement boundary.  `ws004-p031`/HW-T25 supersedes the scheduler shape with
per-endpoint concurrency and request-local unlink while retaining these
controller-observed retirement guarantees.  The focused model and
production-source gate cover completion versus dequeue,
timeout versus IRQ, late/duplicate acknowledgement, quiesce/stop during
retirement, failure retention, exactly-one terminal publication, callback
execution after the ownership lock is released, and worker-callback
enqueue/dequeue re-entry. UHCI additionally covers all 1024 frame entries,
raw-register health before `0x7ff` to zero FRNUM wrap, and successful-TD toggle
progress. EHCI clears a stale IAA before IAAD, accepts only the matching
post-doorbell acknowledgement, and commits the stable QH overlay toggle.

The runner executes ordinary, ASan/UBSan, GCC analyzer, source-contract, amd64
UEFI, and i386 PC/AT production-object gates.  `/tmp` is not used because it
may be a small tmpfs:

```sh
mkdir -p build/q041-tmp
TMPDIR="$PWD/build/q041-tmp" \
  plan/ws004-hardware/tests/run-legacy-hcd-retirement-test.sh
```

The runtime runner uses the dedicated
`config-amd64-legacy-hcd.mk` selection, one disposable OVMF/q35 IDE-root copy,
and a read-only auxiliary USB disk. It boots once with `piix3-usb-uhci` and
once with `usb-ehci`, requires each checked-retirement marker, completes a
4-KiB guest bulk read into disposable tmpfs, then requires clean reboot/HCD
shutdown:

```sh
TMPDIR="$PWD/build/q041-tmp" make -j16 \
  ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-amd64-legacy-hcd.mk \
  disk-image
TMPDIR="$PWD/build/q041-tmp" \
  plan/ws004-hardware/tests/run-legacy-hcd-qemu.sh \
  build/amd64/hdd-image.img build/data.img build/q041-p016-qemu
```

The legacy drivers do not yet dispatch runtime root-port changes, and QEMU
does not provide a stable public fault control for a frozen UHCI FRNUM or
stale, duplicate, or missing EHCI IAA. Cancellation and those faults therefore
remain deterministic focused-model evidence. The QEMU metadata records each
under `not_injected` and never claims hot-unplug or hardware fault-injection
coverage; the runtime portion proves real control/bulk traffic and checked
shutdown only.

## HW-T10 xHCI evidence

The focused arithmetic fixture covers scratchpad decoding, 64-KiB-safe Normal
TRB splitting, USB2/USB3 speed flags, interrupt interval encoding, short-packet
lengths, transfer-event ownership, Link TRB chain/cycle wrap at 254→0, and the
cancellation DMA-retention rule:

```sh
cc -std=c11 -Iinclude -Wall -Wextra -Werror \
  plan/ws004-hardware/tests/xhci-model-test.c \
  -o /tmp/ws004-xhci-model-test
/tmp/ws004-xhci-model-test
```

The USB-storage SCSI fixture covers fixed and descriptor sense decoding plus
the MODE SENSE(6) write-protect bit used by HW-T12 failure injection:

```sh
cc -std=c11 -Iinclude -Wall -Wextra -Werror \
  plan/ws004-hardware/tests/usb-storage-scsi-test.c \
  -o /tmp/ws004-usb-storage-scsi-test
/tmp/ws004-usb-storage-scsi-test
```

The production-path empty-reader fixture supplies current `02/3a/xx` sense to
the real `usb-storage.c` attach routine. It requires one readiness attempt, an
idle bound interface retaining its three preallocated URBs, no disk
publication, and clean detach/free. Deferred sense and a non-removable LUN
remain failures. The runner executes ordinary, ASan/UBSan, and analyzer modes:

```sh
TMPDIR="$PWD/build/q047-tmp" \
  plan/ws004-hardware/tests/run-usb-storage-no-media-test.sh
```

The i386 build selection is `tests/config-pcat-xhci.mk`. Runtime acceptance and
the exact QEMU command are recorded in
[qemu-xhci-evidence.md](qemu-xhci-evidence.md).

## HW-T11 USB boot/root continuity

`ws004-p005` owns this matrix. It must use production bootloader and kernel
paths, not a test-only disk pointer:

- legacy BIOS and supported x64 UEFI boot with the system image attached only
  through q35 `qemu-xhci` USB mass storage;
- the same cases with a non-boot IDE disk and another USB disk attached in both
  orders;
- exact selection by the approved MBR/GPT identity, with zero and duplicate
  matches rejected rather than resolved by discovery order;
- delayed device discovery inside the declared bound and missing media beyond
  it, both with visible diagnostics and no infinite wait;
- login followed by sustained reads and a bounded write/sync/readback on a
  disposable copy of the image; and
- clean reboot plus at least one disconnect/reset error observation without
  claiming physical Latitude completion.

Every evidence record includes the QEMU version, complete command line, image
identity values, highest WS003 U-tier reached, and first failing transition.
The current BIOS/UEFI, ordering, delayed/missing-media, I/O, and reboot findings
are recorded in [qemu-usb-root-evidence.md](qemu-usb-root-evidence.md). Identity
and bounded discovery pass. HW-T12 write/read-only-injection evidence is
recorded by `ws004-p006`; HW-T13 diagnosis and repeated reboot evidence is in
[qemu-warm-reset-evidence.md](qemu-warm-reset-evidence.md).

## HW-T12 USB overlay write stress

The intermittent write failure is evaluated with a phase-owned harness under
this directory. The harness contract is:

- build once with `make -j16`, record the diagnostic fingerprint and SHA-256 of
  the pristine raw image, and never boot that base image directly;
- create a byte-identical disposable raw copy for every iteration;
- boot sequentially with the user's q35, `qemu-xhci`, USB-storage, SMP=4,
  NE2000 topology and a bounded runtime;
- capture the mirrored port `0xe9` debug console to a per-run text file, require
  the expected build fingerprint and `login:`, and retain a post-login settling
  interval;
- classify `loop1`, UFS/FAT, BOT/SCSI, xHCI, or syslog write errors as failures
  even if a prompt is usable;
- classify a kernel fault separately from USB/storage errors, and classify
  missing/truncated evidence, an early QEMU exit, or timeout as harness failure
  rather than pass; and
- retain an aggregate machine-readable result plus the complete log and image
  for each failure.

The harness must first demonstrate that it detects at least one known failure
on the unfixed baseline. After a correction, all 500 classified pristine-copy
boots must pass. Any matching failure after a code change resets the post-fix
count. Runs are sequential because parallel QEMU instances would change host
scheduling and confound the timing comparison. The 500-run threshold was
approved on 2026-08-26; detailed manual acceptance is recorded separately.

OCR is not the primary oracle. The amd64 console mirrors each character to both
VGA and port `0xe9`; exact debug-console text is therefore less lossy. A QEMU
screen capture and OCR result may accompany a failed run to demonstrate visual
parity, but cannot replace the text log.

Separate focused tests must deterministically cover:

- legacy terminal-status-before-length publication producing success with a
  stale zero length;
- corrected release/acquire publication over many iterations; and
- normal completion racing timeout/cancel without terminal-state overwrite,
  premature URB free, or premature DMA release.

The completion-publication and single-terminal-owner model is:

```sh
cc -std=c11 -Wall -Wextra -Werror -pthread \
  plan/ws004-hardware/tests/usb-urb-publication-test.c \
  -o /tmp/ws004-usb-urb-publication-test
/tmp/ws004-usb-urb-publication-test
```

The q009 correction and interrupted 36-run acceptance attempt are recorded in
[q009-hwt12-evidence.md](q009-hwt12-evidence.md). Thirty-five boots were clean;
one independent SMP heap fault blocked the then-1,000-run gate. That historical
sample is not an HW-T12 pass; q010 corrected the blocker and passed the revised
500-run gate.

## HW-T12 SMP heap blocker

`ws004-p008` verifies the allocator's split/merge/realloc/alignment invariants
and the requirement that libc compatibility allocation and `kern_malloc` share
one lock domain:

```sh
cc -std=c11 -I. -Wall -Wextra -Werror \
  -Dmalloc=zed_test_malloc -Dcalloc=zed_test_calloc \
  -Drealloc=zed_test_realloc -Dfree=zed_test_free \
  -c libc/heap.c -o /tmp/ws004-heap.o
cc -std=c11 -I. -Wall -Wextra -Werror -pthread \
  plan/ws004-hardware/tests/kernel-heap-lock-test.c \
  /tmp/ws004-heap.o -o /tmp/ws004-kernel-heap-lock-test
/tmp/ws004-kernel-heap-lock-test
```

The amd64 kernel ELF must also define strong `__libc_heap_lock` and
`__libc_heap_unlock` symbols whenever libc allocation remains reachable. A
weak/no-op hook is a failure when `heap_active_set()` points libc allocation at
the kernel heap. The current linked amd64 kernel has no standard `malloc/free`
call site; q010 evidence is in
[q010-hwt12-evidence.md](q010-hwt12-evidence.md).

## HW-T14 USB function model

The fixture drives the production USB core through a fake HCD. Its baseline
covers two configurations, and its RTL8156-shaped case retains three
vendor/NCM/ECM configurations. Together they cover a storage-compatible
alternate-zero interface, optional IAD plus mandatory CDC Union association,
data alternates zero and one, exact endpoint
publication, sibling claim contention, checked transition rollback, automatic
LANGID discovery, UTF-16LE conversion, and malformed descriptors and strings:

```sh
mkdir -p build/q027-tmp
cc -std=c11 -Iinclude -Iinclude/uapi -Wall -Wextra -Werror \
  -pthread \
  src/drivers/usb.c \
  plan/ws004-hardware/tests/usb-function-model-test.c \
  -o build/q027-tmp/usb-function-model-test
build/q027-tmp/usb-function-model-test
```

## HW-T15 concurrent xHCI URBs

The production-contract model owns requests by slot and DCI, and identifies a
completion by exact wrap-aware submitted TRB membership. It exercises control,
CDC notification, bulk RX, bulk TX, and an independent storage request through
all 120 completion orders. It also covers same-endpoint `EBUSY`, malformed and
cross-endpoint events, cancellation without premature ring reuse, device-only
drain, the HCHalted/bus-master/IRQ global drain boundary, and the explicit
8-KiB reclaim reserve. Ordinary NCM requests remain on the dynamic path.

The runner executes the fixture normally, with ASan/UBSan, with GCC
`-fanalyzer`, and audits the production xHCI/USB/storage sources for the frozen
ownership and reserve interface.

The same gate also runs the production USB-core fixture with a deliberately
blocked asynchronous callback.  It proves that `drv_usb_urb_drain()` joins HCD
ownership after callback return (not merely terminal status publication), that
timeout leaves the URB/callback graph owned and retryable, and that the opaque
HCD capability query reports concurrent URBs only for an advertising HCD.  It
also covers idle and failed-submit drains plus unknown capability rejection:

```sh
mkdir -p build/q027-tmp
TMPDIR="$PWD/build/q027-tmp" \
  plan/ws004-hardware/tests/run-xhci-concurrent-urbs-test.sh
```

## HW-T17 CDC NCM wire and integrated driver

The wire runner exercises the production NTH16/NDP16 negotiation, encoder, and
decoder in ordinary and sanitizer modes. The integrated-driver runner includes
the production class-driver source and a fake concurrent HCD/function. It
covers strict descriptor association, the alt-1 final attach commit, provisional
attach cleanup, persistent notification/RX/TX requests, multiple RX datagrams,
TX ownership, carrier notification, bounded rearm failure, shutdown, forced
detach, normal detach retry with exact graph retention, twelve reconnects, and
isolation from an independent pending Storage request.

It also pauses an admitted network poll while detach begins, proving that alt 0
cannot precede worker retirement. The failed-attach fixture makes cleanup fail
once, verifies that the provisional graph remains intact, then completes one
forced cleanup without a double release.

Q054 extends the production net-device and NCM fixtures through asynchronous TX
accounting. An accepted frame increments `tx_packets` and `tx_bytes` once;
later `STALL`, `TIMEOUT`, `DISCONNECTED`, or `IO_ERROR` completion increments
`tx_errors` exactly once without changing `tx_dropped`. Administrative
`CANCELLED` completion during close, detach, or shutdown is not an error or a
drop. The cases cover callback-before-poll publication, close after a published
error, forced detach with pending accepted TX, quarantine and checked cleanup
retry, and twelve fresh reconnect generations without counter or busy-state
leakage. The common net-device fixture separately proves locked error updates
while the device is live and while checked removal retains the object.

```sh
plan/ws004-hardware/tests/run-net-device-hotplug-test.sh
plan/ws004-hardware/tests/run-usb-cdc-ncm-wire-test.sh
plan/ws004-hardware/tests/run-usb-cdc-ncm-driver-test.sh
```

The integrated runner executes ordinary and ASan/UBSan builds plus GCC
`-fanalyzer`; its q054 result is 2,013 checks in each runtime mode. The retained
USB binding, concurrent-xHCI, USB function, checked-recovery, and shutdown-order
regressions also pass; their current focused counts include 971 binding checks,
1,496 USB-function checks, and 1,111 checked-recovery checks in each applicable
ordinary and sanitizer mode. Configured amd64 and i386 production objects and
the repository `make -j16` build remain required gates.

The final q054 runtime gate passes with fresh private configured amd64 image
SHA-256
`0c794540d535c9a83006428683a16db4d4ffc949b457819401ce00938a7d187c`.
A disposable 4-GiB, 4-CPU OVMF `q35` guest boots solely through `qemu-xhci`
USB Mass Storage, mounts the overlay root, activates swap, starts init, and
reaches the exact `login:` marker in 13 seconds. This is a boot/storage
integration smoke test, not a claim of physical NCM traffic. A real NCM role
or `g_ncm` gadget still owns the physical link/transfer/reconnect acceptance in
WS005 NET-T40.

## HW-T19 RTL8156 NCM association

The production USB-core fixture retains an unsupported vendor configuration,
an IAD-less NCM configuration with a valid Union, and an ECM configuration.
It proves descriptor-score selection of the NCM configuration, successful
control-interface binding and data-interface claim, and bounded configuration
and binding diagnostics. The production NCM-driver fixture separately proves
that the same Union-only association publishes exactly one `ue0`; malformed,
missing, duplicate, overlapping, and contradictory association metadata is
rejected. The current automatic results are 1,404 USB-core checks and 1,283
NCM-driver checks in each ordinary and sanitizer mode, plus both analyzers.

The final p018 gate is one Latitude boot and one adapter insertion. It verifies
only selected NCM binding and `ue0` publication. Link, DHCP, data transfer,
reconnect, and repeated hardware reliability remain WS005.

## HW-T22 CDC ECM QEMU baseline

The production-source ECM fixture directly exercises the independent class
driver with a fake concurrent HCD. It covers strict Header/Union/Ethernet
descriptor binding, optional and contradictory IADs, RNDIS rejection, MAC and
endpoint bounds, packet-filter ordering, raw-frame RX/TX, carrier/speed
notifications, exact-MPS zero-packet requests, attach unwind, close, shutdown,
detach retry, twelve reconnects, concurrent Storage ownership, and callback,
poll, and drain races. It runs ordinarily, with ASan/UBSan, and through the
production-driver analyzer:

```sh
plan/ws004-hardware/tests/run-usb-cdc-ecm-driver-test.sh
```

The terminating-zero-packet gate covers the HCD contract needed by CDC ECM
bulk Ethernet frames whose positive length is an exact endpoint-packet
multiple. It proves that the flag affects only non-control bulk OUT transfers;
control, IN, interrupt, zero-length, and non-multiple transfers remain
unchanged. The xHCI model covers the chained payload TRB, final zero-length
Normal TRB with IOC, TD Size, and usable-ring bound. The EHCI and UHCI models
cover the extra descriptor, final-only IOC, packet-toggle progression, and
descriptor-count bounds.

The runner executes the model normally, with ASan/UBSan, with GCC
`-fanalyzer`, and audits the three production HCD implementations:

```sh
plan/ws004-hardware/tests/run-usb-hcd-zero-packet-test.sh
```

The Noct QEMU harness builds private production images and runs four fresh
UEFI cells: IDE/static, IDE/DHCP, shared-xHCI USB-root/static, and
shared-xHCI USB-root/DHCP. Each cell verifies standards configuration
selection, `ue0`, carrier, ping, counters, detach, and a second generation,
and retains command, hashes, logs, and pcap evidence:

```sh
make -j16 toolchain
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws004-hardware/tests/qemu-usb-cdc-ecm.noct \
  "$PWD" /tmp/ws004-p019-evidence
```

## HW-T20 NVMe QEMU

The p022 admin fixture exercises production register/CAP/queue/Identify
arithmetic, malformed completion ownership, sparse active NSIDs, and namespace
profile rejection. The lifecycle fixture uses the production cleanup ledger
and injects every acquisition and cleanup failure, including quarantine,
retry, exact reverse release, and double-release prevention. Both runners
execute ordinary, ASan/UBSan, and GCC analyzer variants:

```sh
plan/ws004-hardware/tests/run-nvme-admin-test.sh
plan/ws004-hardware/tests/run-nvme-lifecycle-test.sh
```

The p023 fixtures add exact Set Features/Create Queue/NVM command encodings,
64-bit namespace bounds, one-PRP bounce limits, split sizing, status-to-errno
translation, foreign completions, and the serialized BIO/command/DMA ownership
ledger.  The latter injects timeout, reset recovery, failed quiesce quarantine,
shutdown with an in-flight command, late completion, and exactly-once BIO and
resource release:

```sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/run-nvme-io-test.sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/run-nvme-io-lifecycle-test.sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/run-nvme-shutdown-lifecycle-test.sh
```

The existing PCI message fixture additionally verifies that checked MSI and
MSI-X removal restores the exact saved capability/address/data and MSI-X table
entry rather than leaving an OS-programmed vector behind:

```sh
cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  src/drivers/pci.c plan/ws004-hardware/tests/pci-msi-test.c \
  -o build/q030-p022/host/pci-msi-test
build/q030-p022/host/pci-msi-test
```

The p022 QEMU gate boots an OVMF/q35 guest from a disposable IDE copy, attaches
one 32 MiB standard PCI NVMe namespace, and requires exactly one truthful
`/dev/nvme0n1` publication plus login. SHA-256 before/after checks prove that
neither the source boot image nor the discovery-only namespace was modified:

```sh
plan/ws004-hardware/tests/qemu-nvme-admin.sh \
  build/amd64/hdd-image.img build/q030-p022/nvme-final-4
```

The exact commands and final p022 observations are retained in
[q030 NVMe admin evidence](q030-nvme-admin-evidence.md). HW-T20 remains open as
the shared family identifier until p023 adds NVM I/O and p024 completes its
integrity, reset, concurrency, and strict GPT portions.

The p023 destructive runtime gate must use a disposable raw namespace and a
phase-owned monitor/sendkey harness.  Its guest write path must call `fsync()`
on the still-open raw NVMe descriptor before rereading and restarting; a plain
`dd` followed by a process-wide `sync()` is not evidence that this unmounted
devfs descriptor issued the driver's `BIO_FLUSH` operation.

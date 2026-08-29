# WS004 shared test cases

Parent: [WS004](../ws.md)

| Case ID | Area | Required observation |
| --- | --- | --- |
| HW-T00 | PCIe/DMA/interrupts | BARs, capability walking, DMA widths/order, MSI/MSI-X setup/teardown, and timeout cleanup pass focused tests |
| HW-T01 | ECAM/MSI HAL contract | Canonical source parsing, MCFG validation, vector allocation/exhaustion/reuse, PCI register images, rollback, in-flight unregister, and real QEMU delivery pass |
| HW-T02 | Legacy PCI HCD IRQ teardown | EHCI and UHCI retain the IRQ cookie/allocation, DMA, BAR-or-I/O ownership, HCD bus, handler argument, and controller after checked removal failure; retry releases each resource exactly once |
| HW-T10 | xHCI model | QEMU enumeration, control/bulk/interrupt transfers, reconnect, timeout, and controller reset pass |
| HW-T11 | USB storage | Root-continuity cases from WS003 pass through xHCI |
| HW-T12 | USB overlay writes | Correlated URB/heap tests pass; 500 sequential q35/xHCI/SMP=4 boots from pristine raw-image copies have zero kernel/storage-error markers; explicit `DATA.IMG` persistence and IDE control pass; detailed manual acceptance follows |
| HW-T13 | PC/AT warm reset | Three consecutive guest reboots reach fresh login with clean kernel BSS state |
| HW-T14 | USB function model | Multiple configurations, IAD/Union, class-specific descriptors, alternate endpoints, strings, sibling claims, and failed-selection rollback pass production-source fixtures |
| HW-T15 | Concurrent xHCI URBs | Control, interrupt, bulk RX/TX, and independent storage requests complete out of order; isolated cancel, quiesce, and reclaim reserve invariants pass |
| HW-T16 | Removable net device | Carrier changes, queued-RX detach, ARP/route purge, deferred release, shutdown, and more than eight reconnects pass without stale ownership |
| HW-T17 | CDC NCM wire and driver | NTH16/NDP16 valid/malformed fixtures, strict negotiation, bind/unwind, RX/TX, notification, detach, reconnect, and concurrent storage pass |
| HW-T18 | USB binding transactions | Idle inactive-alt URBs, interface-scoped switch admission, sibling concurrency, submit races, provisional attach abort, detach retention, EP0 serialization, and Mass Storage regression pass |
| HW-T19 | RTL8156 NCM association | An unsupported-vendor/NCM/ECM three-configuration fixture selects Union-associated NCM without requiring an IAD; matching IAD corroborates, contradictions reject, binding diagnostics identify selection/outcome, and one final Latitude insertion publishes `ue0` |
| HW-T20 | NVMe QEMU | Identify, namespace bounds, read/write/flush, concurrency, reset, and failure tests pass on disposable images |
| HW-T21 | NVMe hardware | Read-only identify precedes explicitly safe I/O; device IDs and stress/error logs are stored |
| HW-T22 | CDC ECM QEMU baseline | A QEMU RNDIS-first/ECM-second `usb-net` function selects ECM without a quirk, publishes `ue0`, carries raw Ethernet and DHCP/ping traffic, detaches cleanly, and coexists with xHCI USB Storage |
| HW-T23 | CDC NCM deterministic hardening | Arbitrary first and mismatched fully valid sequences deliver and resynchronize, malformed NTBs preserve state, zero-delivery completions consume bounded poll work, and each open programs the packet filter after the active alternate and before input URBs |
| HW-T24 | xHCI SuperSpeed interrupt context | Companion `wBytesPerInterval` validation and host-endian decode produce exact Max ESIT/Average TRB Length fields for RTL8156 EP3; zero, oversized, reserved-field, 16384-byte ceiling, and 16385 rejection cases behave as specified while non-SS/non-interrupt contexts remain unchanged |
| HW-T30 | WLAN logic | Scan/association/key/error state tests pass against a bounded fixture without claiming radio hardware success |
| HW-T31 | WLAN hardware | Firmware load, scan, authentication, association, reconnect, and data-plane tests pass on the exact device |
| HW-T40 | i915 foundations | Device-independent UAPI/model tests pass; modeset/scanout/reset require target-hardware evidence |

QEMU/model and physical-hardware results are always separate evidence fields.

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

```sh
plan/ws004-hardware/tests/run-usb-cdc-ncm-wire-test.sh
plan/ws004-hardware/tests/run-usb-cdc-ncm-driver-test.sh
```

The integrated runner executes ordinary and ASan/UBSan builds plus GCC
`-fanalyzer`; its q028 result is 1,283 checks in each runtime mode. This is an
automatic software gate. A real NCM role or `g_ncm` gadget still owns the
physical link/transfer/reconnect acceptance in WS005 NET-T40.

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

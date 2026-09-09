# WS009: documentation

Last updated: 2026-09-09

WSID: `ws009`

Status: active; p007 completed current architecture/UAPI reconciliation in
q150. p008 published boot/storage guidance but remains uncleared for the
full native/installed-NVMe procedure dependent on WS019-p004/p005.

Parent: [master plan](../master.md)

Last verified Phase: `ws009-p007` complete (`q150`)

Resume point: complete producer-dependent native/installation instructions and
their guide acceptance after WS019-p004/p005. DOC-54 stays with the manually
held GPU producer; current documentation explicitly records its absence.

Shared tests: [WS009 test index](tests/README.md)

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws009-p001`](phase001-information-architecture/phase.md) | Complete | Product hierarchy/rules and Noct relative-link validation established |
| [`ws009-p002`](phase002-build-guide/phase.md) | Complete | Toolchain, parallel image build, amd64 QEMU marker, diagnostics, and links pass |
| [`ws009-p003`](phase003-init-services/phase.md) | Complete | Current init/service configuration, readiness, supervision, and shutdown contract published |
| [`ws009-p004`](phase004-evdev-input-reference/phase.md) | Complete (`q046`) | Current multi-source, momentary, resync, console-subscriber, and detach behavior is documented without claiming live USB HID |
| [`ws009-p005`](phase005-kernel-parameter-reference/phase.md) | Complete (`q046`) | Common parameters and all four required configured x86 loader paths are reconciled with production source and retained q015/q031/q032 evidence |
| [`ws009-p006`](phase006-completed-producer-follow-up/phase.md) | Complete (`q072`) | Current WLAN, physical USB HID, Intel Mac Variant, Noct 2.0.1, and project-toolchain behavior is published without claiming planned removals or new UAPIs |
| [`ws009-p007`](phase007-current-architecture-uapi/phase.md) | Complete (`q150`) | Architecture, compatibility, console/graphics/system and multi-radio WLAN reconciled; 283 product links pass |
| [`ws009-p008`](phase008-boot-install-guides/phase.md) | Uncleared (`q150`) | Boot/root/USB limits published; full installation and native preparation guide waits for WS019 acceptance |

Documentation work may be extracted alongside a producer WS when it describes
a newly frozen public interface, but it retains a WS009 Phase ID and acceptance
record.

## Goals

- Explain the zedBSD architecture, independent specifications, HAL boundaries,
  public UAPIs, build system, boot flow, init/services, and supported devices.
- State current behavior, compatibility, limitations, and planned behavior
  without presenting one as another.
- Make a clean build, QEMU boot, and supported-system administration
  reproducible from the documentation.

## WS completion conditions

WS009 is complete when every listed documentation work item is published,
cross-linked, and traceable to current headers/source/tests; build and boot
instructions are reproduced in a clean environment; compatibility claims agree
with WS001; and the documentation link/structure checks pass.

## 1. Objective

Create maintainable documentation for zedBSD's independent specifications,
architecture, public interfaces, build/boot process, init system, and HAL
bridges. Documentation is developed alongside stable behavior and is required
evidence for new public UAPIs.

Plans remain under `plan/`. Product documentation may later be organized
under directories such as `docs/architecture/`, `docs/reference/`, and
`docs/howto/`; that layout is chosen in the first documentation Phase rather
than assumed here.

## 2. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| DOC-00 | Complete | Documentation information architecture, style, version/status banners, and link checks | Current docs inventory | Navigation and automated relative-link validation pass |
| DOC-10 | Complete (`q150`) | zedBSD independent-specification overview | Architecture inventory | Each intentional divergence has rationale, stable contract, and implementation references |
| DOC-11 | Complete (`q150`) | HAL overview and architecture-independent kernel structure | Kernel/platform audit | Subsystem boundaries, ownership, and amd64-specific examples are traceable to source |
| DOC-12 | Complete (`q150`) | UAPI POSIX/SUS compliance and `_XOPEN_SOURCE` profile | PX-02 | Claims match headers, implementations, and compliance ledger |
| DOC-20 | Complete | Complete build-from-source guide | Supported toolchain/build audit | Toolchain/image commands reproduced; amd64 QEMU and links pass |
| DOC-30 | Published (`q150`; retained boot evidence) | Bootloader and boot-flow guide | BR-01–BR-04 design | BIOS/UEFI flow, failure points, and diagnostic paths are documented |
| DOC-31 | Complete through `ws009-p005` | Kernel parameter reference | Parser/default audit | Every documented parameter cites parser/default and unknown-key behavior |
| DOC-32 | Published (`q150`; retained boot evidence) | Boot filesystem plus loopback-root guide | Boot/root implementation | QEMU procedure reaches the intended root reproducibly |
| DOC-33 | Partial / uncleared (`q150`) | Native UFS-root guide | Stable native storage drivers | QEMU and supported hardware procedures are explicit and safe |
| DOC-34 | Partial / uncleared (`q150`) | USB trial and NVMe installation guide | WS019 p005, WS003 p018 | USB-first evaluation, the existing-GPT/ESP/FAT32 prerequisite, read-only `diskpart`, no-format overlay `zedinst`, fallback/manual UEFI selection, and recovery limits match the released first-stage tools; native/destructive guidance waits for its later Phase |
| DOC-40 | Complete | zedBSD init, rc.conf, service.d, fd 3 readiness, and shutdown model | Phase 11–20 implementation | Boot/service/shutdown examples match tested behavior |
| DOC-50 | Complete (`q150`) | `/dev/console` HAL-bridge reference | IN migration state | Text/input/framebuffer roles and deprecated interfaces are accurate |
| DOC-51 | Complete (`q150`) | `/dev/graphics` HAL-bridge reference | GFX takeover design | Framebuffer, mmap/ioctl, ownership, and GPU takeover are specified |
| DOC-52 | Complete (`q150`) | `/dev/system` HAL-bridge reference | Device/UAPI audit | Operations, permissions, data structures, and architecture hooks are specified |
| DOC-53 | Complete through `ws009-p004` | evdev and `/dev/input` UAPI reference | IN-00 through the q044 p006/p007 boundary | ABI profile, event semantics, examples, current ownership, and compatibility differences are published without claiming live USB HID |
| DOC-54 | Producer manual hold; absence documented (`q150`) | `/dev/gpu` UAPI and capability reference | GFX-10 | Versioned object model and supported profiles match headers/tests |
| DOC-55 | Complete (`q150`) | `networkd`, `net`, `dhcpc`, WPA backend, and rc.conf networking reference | NET-20+ as applicable | Command/config/protocol/error examples pass tests |

## 3. Required document qualities

Public-interface documentation includes:

- stability/version status and supported architectures;
- exact headers, structures, constants, calls/ioctls, error behavior, and
  permissions;
- ownership, lifetime, concurrency, blocking, and cancellation rules;
- normal example plus timeout/failure example;
- implementation and test references;
- compatibility statement for POSIX, SUS, Linux, FreeBSD, or zedBSD-specific
  behavior as applicable.

Architecture documents distinguish current implementation from intended design.
Planned behavior is not presented as already available.

## 4. Delivery sequence

1. DOC-00 establishes navigation and validation.
2. The build and minimum boot/diagnostic guide is produced early enough to
   support external reproduction of hardware bring-up.
3. Init/network documentation consolidates the already implemented Phase 11–20
   behavior.
4. evdev and GPU references are written as part of their UAPI design Phases,
   before legacy input removal or accelerated consumers are declared stable.
5. The POSIX/SUS profile is synchronized with the compliance master on every
   relevant Phase completion.

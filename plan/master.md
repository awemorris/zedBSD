# zedBSD master plan

Last updated: 2026-08-31

Status: active

## 1. Purpose

This is the authoritative program-wide plan. It assigns permanent workstream
IDs, records the current stop/resume point of every WS, and defines dependencies
between WSs. Detailed scope belongs in each `ws.md`; implementation design,
acceptance, results, and interruption state belong in each `phase.md`.

Observed defects deliberately deferred from the active Queue are maintained in
the [known-bug ledger](known-bugs.md).  Their presence does not weaken a Phase's
declared acceptance boundary, and they return to implementation only when a
later Queue selects them explicitly.

The completed first hardware north star was:

> Boot zedBSD from USB on a Dell Latitude 5320, reach a usable local shell, and
> establish a working network path with reproducible evidence.

The next hardware/install north star is deliberately staged:

> Implement NVMe, use `/bin/zedinst` from the ordinary USB system to place one
> file-backed overlay installation on already existing GPT/FAT32 storage, and
> boot that installation on the Dell Latitude 5320 through `BOOTX64.EFI`.

Native-root installation, GPT creation, and filesystem creation follow as a
separate milestone after this non-formatting path is accepted.

Panasonic CF-SV7 is the second declared laptop bring-up target. `ws003-p020`
cleared its original post-RSDP early ACPI/interrupt stop. `ws003-p021` then
made the fixed 397,312-sector GPT image safely usable after raw copying to the
60,549,120-sector USB device. Its final physical observation boots
successfully through the USB-root overlay and closes that root-continuity
boundary without reopening the completed Latitude USB/network evidence.

NEC PC-9821V13 is the declared native PC-98 physical target. The observation
which triggered q037 beeped before loader output; p022 owns that early-IPL regression
without replacing the PC-98 partition scheme with a PC/AT MBR.

The completed q044 input milestone makes every current physical or
character-only producer source-owned, routes console translation through a
bounded internal subscriber, and repairs overflow through an atomic
begin/snapshot/end state transaction. Its independent bounded HID parser
rejects impossible field widths even on unsupported usages and passes 791
strict/sanitizer/analyzer checks. Live USB HID attachment remains WS006 p008;
fresh image/QEMU evidence is presently held by the already recorded WS008
Noct CLI incompatibility.

The archived [q039](queue-q039.md) first proves the
already-correct PC-98 `IPL1` field and has prepared one immutable audio-trace
image for the still-silent PC-9821V13 boundary; its automatic gates pass and
one physical observation remains. It then repairs the independently
reproduced PC-98 PIC-cascade defect which blocked Xzed mouse events; the
production qemu-pc98 cursor now moves by the exact injected delta through
evdev without monitor-side PIC repair. A later comparative audit has isolated
Stage 1's unused SENSE-before-fixed-read sequence as the narrowest remaining
V13 compatibility difference. q043 removes only that transaction while
preserving PBR/BOOTZBSD geometry checks; its normal and diagnostic qemu-pc98
paths reach login and exact artifact `7d4e7d67...` awaits one V13 boot. q043 also freezes
the newer PC-98 Xzed mouse report: the final image and maintained headless QEMU
path again move the cursor exactly `(320,240) -> (420,290)`, and every focused
host gate passes. Because that QEMU exposes no interactive display backend,
`ws007-p004` remains honestly `uncleared` for the user's exact failing GUI
environment rather than receiving a speculative repair. The archived [q040](queue-q040.md)
records the uncleared printed-label-only Archer identity checkpoint and the
completed independent AF_UNIX/network authorization foundation. The current
[Queue Book](queue.md) records q044's completed input ownership and bounded
HID parser milestones. The archived q043 PC-98 fixed-read and exact-mouse
paths both pass locally, and their remaining external resume facts do not
block subsequent Queues. The
archived [q038](queue-q038.md) replaced the
stale or intermediate image used for the first Intel Mac observation with one
fresh, production-checked UEFI-only artifact and passed the exact larger-USB
path through init/login. One provisional p004 boot of that exact hash is next.
The archived [q037](queue-q037.md) passed every automatic native PC-98 IPL and
INT 1Bh disk-read gate while retaining the native partition layout and `55 aa`;
its frozen image still awaits the already-requested PC-9821V13 observation.
The archived [q036](queue-q036.md) completed the generic Variant and three
fixed amd64 image-layout Phases through their automatic layout gates. Its
strict six-cell runtime Phase is uncleared because three fresh runs exposed an
image/firmware-independent init/getty scheduling flake after root and swap had
already succeeded; the oracle was not weakened.
The archived [q035](queue-q035.md) completed the retained WS018 graphics
runtime matrix, consolidated FAT, migrated FAT boot-media access to native VFS
objects, and removed the retired bootfs/namespace/startup/M9 and broad
internal-state graph. Six empty supported-manifest builds, the ordinary image
build, focused host gates, and four production x86 boot paths passed; WS018 is
complete. The archived [q034](queue-q034.md) completed
`ws003-p021`. Its generic bounded-GPT implementation, corruption fixtures,
larger-media SeaBIOS/OVMF USB boots, ordinary regressions, and final CF-SV7
boot pass. The accepted contract is general: a coherent GPT-declared end may
precede the physical disk end; the remaining sectors are unallocated rather
than an error. A declared end beyond physical capacity is rejected. Archived
[q033](queue-q033.md) completed `ws003-p020`: its implementation,
host/negative fixtures, BIOS USB gate, OVMF 4/8/16-GiB matrix, and one CF-SV7
observation pass.
The archived
[q032](queue-q032.md) made i386 PC/AT and amd64 BIOS `BOOTZBSD.EXE` consume
`/zedbsd.cfg`, made PC-98 `BOOTZBSD.EXE` consume the same language through
`/BOOTZBSD.CFG`, and made one amd64 hybrid image use a common payload FAT
through BIOS and UEFI. Completed `q031` removed and
audited the dead UEFI boot path, then replaced the hard-coded amd64 UEFI
launch with required same-disk `/zedbsd.cfg` discovery and direct
kernel-parameter translation. The
completed [q030](queue-q030.md) implemented `ws004-p022` through
`ws004-p024`: the QEMU NVMe
controller/admin/Identify foundation, bounded I/O/read/write/flush lifecycle,
strict primary/backup GPT discovery, and the final disposable acceptance
matrix. Read-only storage administration moves to the next Queue; the
installer and its QEMU acceptance remain outside q031 until live
`DATA.IMG`/`SWAPFILE` copying is replaced by an approved stable-source
contract. After WS019 p004/p005, one combined physical
`ws004-p025`/`ws003-p018` checkpoint follows. `ws004-p020` is
complete: fully valid NCM NTBs accept and resynchronize any sequence, malformed
input preserves state, completion work is budgeted, and packet-filter
programming occurs on open after the active alternate. The safe automatic
`ws005-p001` DHCP/diagnostic slice and disposable QEMU USB-root gate pass. The
first Latitude attempt then stopped at carrier; a real RTL8156 USB capture
proved that valid connection/speed notifications used paired data-interface
`wIndex=1` and were discarded by a control-interface-only check. The bounded
repair passes carrier, a static-to-DHCP transition, routing, NTB traffic, and
ping with that physical adapter through QEMU xHCI. The final Latitude-native
image then completed DHCP and `fetch www.google.com`. This achieves the
immediate USB-boot/local-shell/network north star and completes `ws005-p001`
and q029. CDC ECM and the separately planned xHCI SuperSpeed-interrupt context
Phase were excluded from q029 and remain independent future work.

The archived [q028](queue-q028.md) corrected the IAD-less, Union-associated CDC
NCM match, added concise binding diagnostics, and passed its physical scope when
the Latitude selected configuration 2 and published `ue0`; it did not claim
carrier or packet transfer. The archived [q027](queue-q027.md) completed all six
USB/NCM software Phases, including the
general binding/interface transaction and integrated `ueN` automatic
milestone. Link, DHCP, transfer, reconnect, and repeated physical acceptance
remain in WS005. The archived [q026](queue-q026.md) completed the
disk-label/platform, Xzed-evdev, and input/HID ownership Phases and implemented
the independent graphics ownership change. The graphics Phase remains
`uncleared` only for its explicit residual runtime matrix. The archived
[q025](queue-q025.md) disabled the
target Noct/Remacs package options without touching the maintainer's Noct
source, then completed the first WS018 kernel-ownership foundation wave. The
archived [q024](queue-q024.md) passed automated Noct gates but is recorded
`uncleared` because the Principal Engineer's first source review rejected the
implementation quality. Noct is now under maintainer-only manual repair. The
requested kernel reorganization remains WS018 p001--p012. The
archived [q023](queue-q023.md) completed `ws003-p016`, `ws008-p005`, and
`ws001-p014` in that order without reaching a human-decision boundary. The archived
[q022](queue-q022.md) published the Noct maintainer-review correction as upstream commit
`eba2043ca74b8601d68a405ecbbeca50ca8d5ac0`, replaced the zedBSD gitlink with
one pinned source-acquisition Makefile, and passed host plus non-JIT, BeUI, and
JIT QEMU acceptance. The archived
[q021](queue-q021.md) completed all four WS016 runtime-swap Phases, including
six passing amd64 UEFI runtime and representative boot-regression cells. The
archived [q020](queue-q020.md) completed `ws006-p005`, `ws008-p002`, and
`ws008-p003`, including capability-only evdev discovery, canonical BeUI
graphics/input, direct amd64 RW-to-RX execution, and canonical Noct JIT
lifecycle evidence. The preceding [q019](queue-q019.md) completed
`ws008-p001`, honestly stopped p002 before implementation, and did not start
dependency-gated p003. The
preceding [q018](queue-q018.md) completed `ws012-p003` through p006 and closed
WS012 after ZSV1, argv/persistent policy, the interactive console, and
production integration all passed. [q017](queue-q017.md) completed
`ws012-p002`: the strict YAML rc.conf host
fixtures, production build, and disposable amd64 QEMU persistence/reboot proof
all passed. [q016](queue-q016.md) completed `ws004-p009`.
[q015](queue-q015.md) completed `ws003-p011`--`p015`: BR-T46 passed all 31
production-loader cells across i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64
UEFI in the post-review `q015-br-t46-final-007` run. The earlier
[q014](queue-q014.md)
record completed `ws003-p010` and BR-T41 when one Latitude boot resolved the
USB root, mounted the writable overlay, started init, reached a root shell,
and also reached X/`zterm`. Earlier records include [q013](queue-q013.md),
[q012](queue-q012.md), and
[q011](queue-q011.md). Other closed archived records are
retained as [q001](queue-q001.md), [q002](queue-q002.md),
[q003](queue-q003.md), [q004](queue-q004.md),
[q005](queue-q005.md), [q006](queue-q006.md),
[q007](queue-q007.md), [q008](queue-q008.md), and
[q009](queue-q009.md), and [q010](queue-q010.md).

## 2. Goals

### 2.1 Current project goal

Reimplement the useful operating-system functionality and interfaces found in
commercial UNIX, BSD, and Linux as zedBSD under the most permissive practical
license. The implementation is clean and independent: compatibility is a
behavioral and interface target, not permission to copy proprietary or
incompatibly licensed base-system source.

The zedBSD base system—including the kernel, libc, boot environment, init and
service infrastructure, core command set, system libraries, and native system
tools—is fully reimplemented for zedBSD. External implementations are not
incorporated into the base system.

Software distributed as packages is a separate boundary. Packages may build
and provide GNU software and other third-party software under their respective
licenses. This allows familiar GNU applications and toolchains without changing
the licensing or implementation policy of the zedBSD base system.

### 2.2 Final product goal

Deliver zedBSD as a common operating-system foundation for:

- desktop operating systems;
- mobile operating systems;
- embedded systems, including routers and network appliances; and
- server operating systems.

These products may select different packages, services, drivers, user
interfaces, and security/resource policies while sharing the reimplemented
base system and stable zedBSD interfaces.

### 2.3 Current platform installation milestone

The next bounded platform goal is the first NVMe overlay installation on the
Dell Latitude 5320. zedBSD first implements and accepts its NVMe controller
and block path in QEMU, then identifies the laptop's SanDisk SN740
(`15b7:5015`) read-only. The ordinary USB-booted system provides
`/bin/zedinst`; no separate installer image is introduced. `/sbin/diskpart`
initially supplies read-only GPT inspection only.

Installer v1 neither creates nor edits a partition table and never formats a
filesystem. It requires an existing GPT disk with exactly one usable ESP, then
requires the user to select a distinct existing FAT32 partition on that same
disk. The ESP receives `EFI/BOOT/BOOTX64.EFI`; the selected payload filesystem
receives `vmunix`, `zedbsd.cfg`, `rootfs.img`, `data.img`, and `swapfile`.
Unrelated files and partitions are preserved. Resize, move, GPT creation,
filesystem creation, native root, and general dual-boot assistance are later
work. USB boot remains the recommended way to try zedBSD without modifying
internal storage.

Installer v1 does not create, reorder, or delete UEFI `Boot####` variables. It
installs the standard fallback/recovery pathname and accepts one firmware
menu/file-selection boot on the Latitude. Portable unattended fixed-disk boot
through an explicit firmware entry is a later optional step if the target
firmware does not discover the fallback path.

### 2.4 BIOS boot configuration convergence milestone

All supported x86 BIOS loaders converge on the same direct configuration
format already defined for UEFI. The three explicit targets are:

- i386 PC/AT: active FAT PBR -> `BOOTZBSD.EXE` -> root `/zedbsd.cfg`;
- amd64 BIOS: active payload FAT PBR -> `BOOTZBSD.EXE` -> root
  `/zedbsd.cfg`, replacing the separate reserved-area direct-kernel path; and
- i386 PC-98: native payload PBR -> `BOOTZBSD.EXE` -> root
  `/BOOTZBSD.CFG`.

The PC-98 filename remains different for its legacy environment, but its file
format, bounds, normalization, configured-kernel behavior, and emitted kernel
parameters are identical to `/zedbsd.cfg`. Common behavior is required;
common loader source is not. On a modern hybrid GPT/MBR image, UEFI and BIOS
must boot the same payload FAT and therefore the same configuration and
images. q032 completed this milestone with the PC/AT/amd64 20/20 production
matrix and the PC-98 16/16 production-PBR matrix. Atomic image validation,
per-medium GPT GUID generation, and installed Stage 1/Stage 2 identity
verification also passed. Checker mismatch and process-failure tests preserve
the prior published image, clean every extraction/unchecked sibling, and allow
an immediate clean retry.

### 2.5 Program completion direction

The project advances toward the final goal when the base system can be built
from its own maintained source, boots reliably on supported targets, provides
the documented UNIX/POSIX-oriented interfaces, and supports product-specific
package sets for all four target classes. Individual WSs may complete or pause
before this long-term product goal is reached.

### 2.6 Design preference: interface-based modularity and late abstraction

This subsection is informative rather than a mandatory project rule. It
records the project owner's preferred style so that future design discussions
can evaluate proposals in the intended context.

Modularity is primarily established by a clear, stable interface and by
information hiding, not by maximizing implementation reuse. An implementation
behind that interface should be free to use a different internal structure and
should be replaceable as a whole without changing its consumers. Two modules
that satisfy the same external contract therefore need not share an internal
framework merely because some of their current code looks similar.

Code duplication is not, by itself, considered a design defect or an automatic
refactoring trigger. The preferred approach is AHA/late abstraction: allow
independent implementations to develop first, observe which parts actually
remain common, and extract a shared implementation only after that commonality
has become substantial and stable. When choosing between duplication and a
premature or constraining abstraction, preserving independent implementation
freedom is generally preferred. Shared conformance tests may enforce the
interface contract without requiring the implementations themselves to share
code.

The same preference applies to public headers.  Prefer a small number of
cohesive, deliberately comprehensive interface headers over many convenience
fragments.  A public header is a stable design ledger, not an implementation
scratchpad: additions, removals, or splits should follow a significant and
explicit interface decision, while ordinary implementation refactoring should
adapt below the existing contract.  This is informative project-owner
guidance, like the duplication preference above, rather than a ban on every
future header change.

### 2.6 Project scripting language and bootstrap order

Noct is the project scripting language for repository-owned build, image,
generation, and maintenance tooling. A supported build first runs
`make toolchain`, which obtains and builds the host Noct interpreter below
`build/NoctLang`; subsequent project scripts invoke that interpreter. New
Python dependencies must not be added to supported production build paths,
and an already migrated path must not regress to invoking Python.

The bootstrap needed to obtain Noct may use Make, the host compiler, CMake,
Git, and a minimal shell recipe because Noct does not exist yet at that point.
Ordinary Make recipes that directly invoke tools are not required to be
rewritten as scripts. When a value can be expressed clearly as maintained
source or an ordinary Make dependency, prefer that over generating source at
build time; Noct is the implementation language when a project-owned script is
actually warranted.

## 3. Workstream registry

| WSID | Workstream | Status | Last completed / current Phase | Resume point | WS plan |
| --- | --- | --- | --- | --- | --- |
| `ws001` | POSIX.1-2024 compliance | Active ledger; q042 retained passing source/host milestones for both VFS prerequisites discovered by q041 | `ws001-p014` complete; p015/p016 source/host milestones uncleared | Restore the Noct host CLI through WS008 p010, complete p015's two retained fault cells, then run fresh native/remount acceptance for p015/p016 before returning to WS005 p005 | [WS001](ws001-posix/ws.md) |
| `ws002` | System services | Complete baseline | `ws002-p020` complete with handoffs | New networking work resumes in WS005 | [WS002](ws002-services/ws.md) |
| `ws003` | x86 laptop and PC-98 hardware bring-up | Active; Latitude USB/network and CF-SV7 USB-root milestones complete; q043 p024 fixed-read source/binary/QEMU milestone passes; p018 Latitude overlay-NVMe install/boot remains dependency-gated | p020/p021 physical CF-SV7 path passes; p024 exact artifact `7d4e7d67...` awaits one V13 boot; p018/p019 remain | Record one p024 PC-9821V13 result without first running older artifacts, then return to dependency-ready p018 work; restore its Make-owned Noct gate through ws008-p010 | [WS003](ws003-bringup/ws.md) |
| `ws004` | Hardware expansion | Audited active; q041 p016 completed checked UHCI/EHCI request retirement with configured and QEMU lifecycle evidence. q040 p026 completed every automatic/read-only intake step and is uncleared only for the purchased unit's printed model/region/revision. `q030` NVMe software and the physical USB-Ethernet path are complete | `ws004-p010`--`p016`, `p018`, p020, and p022--p024 complete | Proceed through q041; obtain one serial-redacted Archer label observation when convenient without blocking independent work | [WS004](ws004-hardware/ws.md) |
| `ws005` | Networking and WLAN | Active; q029 p001 and q040 p003 complete; q041 p005 host implementation passes and q042 advanced but did not complete its VFS dependencies | Physical USB Ethernet and authenticated control pass; strict local credential parser/store/CLI now pass host safety and regression gates | Complete WS001 p015/p016 after WS008 p010, then requeue p005 root/non-root ownership and remount-durability acceptance; p004 remains hardware-core dependency-gated | [WS005](ws005-networking/ws.md) |
| `ws006` | Input and evdev | Active; q044 completed truthful per-source ownership and the bounded HID parser, but live USB HID and final legacy removal remain | `ws006-p006` automatic/source and `ws006-p007` parser milestones complete | Execute dependency-ready p008 for live USB HID; run p009 consumer/legacy-console removal only after the accepted WS008 userland tree is available | [WS006](ws006-input/ws.md) |
| `ws007` | Graphics and desktop | Active; q039 PC-98 cascade repair complete, q043 local exact-reproduction matrix passes while the newer GUI report remains external | `ws007-p001` and `p003` complete; p004 uncleared; amd64 `p002` carried | Resume p004 only from the user's exact failing image/QEMU/interactive-backend/focus record; do not alter the passing headless path speculatively | [WS007](ws007-graphics/ws.md) |
| `ws008` | Noct and BeUI | Blocked; target packages remain disabled, and the p008 host pin is not production-usable because upstream removed its documented module-path CLI | `ws008-p008` completed its bounded pin/smoke at `3bf3d236...`; p010 records the later `--path` production-build regression and p009 remains separately target-blocked | Resume p010 from the maintainer's accepted upstream repair or an explicit compatible revision, then resume p009 only after its target blocker is also resolved | [WS008](ws008-noct/ws.md) |
| `ws009` | Documentation | In progress | `ws009-p003` complete | Extract the next dependency-ready producer-linked reference | [WS009](ws009-documentation/ws.md) |
| `ws010` | Noct scripting and x86 image tools | Complete | `ws010-p001`–`p004` complete | Noct toolchain and the 15-script x86 production closure are complete; three images boot to `login:` | [WS010](ws010-scripting/ws.md) |
| `ws011` | Network configuration console | In progress; VLAN/bridge hold released, detailed design still open | `ws011-p003` complete; p004 resumed for design; p005 bounds open | After higher priorities, finish p004's virtual-interface/UAPI/packet-ownership design before implementation; freeze p005 bounds independently | [WS011](ws011-net-config/ws.md) |
| `ws012` | Service administration console | Complete (`q018`) | `ws012-p006` complete | No current Phase; extract a new requirement or continue container integration in WS013 | [WS012](ws012-service-console/ws.md) |
| `ws013` | CPAR container partitioning | Active; q031/q032 configured x86 boot paths complete, Runtime topics manually blocked | `ws013-p002`--`p006` complete | No Boot configuration Phase remains; resume Runtime namespace/CLI/package design only after its explicit manual holds are released | [WS013](ws013-containers/ws.md) |
| `ws014` | Native GPU stack | Blocked by manual hold | `ws014-p001` is blocked before detailed design | Resume only after explicit user release | [WS014](ws014-gpu/ws.md) |
| `ws015` | μITRON asymmetric real-time domain | Blocked by manual hold `MB-007`; user-mode RT direction recorded | `ws015-p001` is the only current Phase | After explicit hold release, select the μITRON profile and freeze the remaining RT/POSIX, mailbox/filesystem, failure, and timing contracts | [WS015](ws015-muitron-rt/ws.md) |
| `ws016` | Runtime swap control | Complete (`q021`) | `ws016-p004` complete; SWAP-T001--T012 and the six-cell amd64 UEFI matrix pass | No Phase remains; extract a new requirement before resuming | [WS016](ws016-swap-control/ws.md) |
| `ws017` | `/dev/graphics` LFB fast path | Planned; p001 blocked on one human `mprotect` decision | No Phase started | Choose the mapping permission ceiling, then Queue p001 device-mmap/UAPI followed by p002--p004 | [WS017](ws017-lfb-graphics/ws.md) |
| `ws018` | Kernel source ownership and interface consolidation | Complete (`q035`) | `ws018-p012` complete; p001--p012 all cleared | No Phase remains; extract a new requirement before resuming | [WS018](ws018-kernel-architecture/ws.md) |
| `ws019` | Installation and disk administration | Re-plan required; installer language changed to Noct | `ws019-p001` retains the approved storage safety contract; older p002--p005 implementation language is superseded pending revision | Do not implement from the old C-oriented Phase map. The latest request ended after `仕様は`; obtain the missing Noct installer contract, then rewrite the bounded implementation/acceptance Phases | [WS019](ws019-installation/ws.md) |
| `ws020` | Intel Mac UEFI bring-up and generic image variants | Active but physical work deferred by user; q036 p001-p002 and q038 p005 complete, p003 uncleared | First physical log used an older three-entry Hybrid-family artifact; fresh fixed UEFI-only hash `3bca88c3...` passes production checking and an exact 60,549,120-sector OVMF/xHCI boot through login | Resume p004 only when the user reselects the Intel Mac checkpoint; separately resolve p003 before its final five-run campaign | [WS020](ws020-intel-mac/ws.md) |

## 4. Milestones

| Milestone | Required result | Owning WSs |
| --- | --- | --- |
| M0 — Baseline preserved | Current QEMU boot, init, login, shell, and service behavior remains usable | WS001, WS002 |
| M1 — QEMU USB root | Automatic milestone complete: identity/reboot, URB and heap corrections, controls, and 500 pristine-copy boots pass; detailed manual acceptance pending | WS003, WS004 |
| M2 — Latitude USB shell | U3 is complete and one BR-T41 boot reached init/login/root shell and X/`zterm`; full U4/U5, BR-T30 repeatability, and BR-T31 sustained root I/O remain | WS003, WS004, WS009 |
| M3 — Latitude network | At least one documented USB Ethernet interface configures and transfers data | WS003, WS004, WS005 |
| M4 — Native platform devices | NVMe, USB HID, and the selected WLAN work on the target | WS004, WS005, WS006 |
| M5 — Application environments | X11 is usable, its optional mapped-LFB path is accepted on amd64 UEFI, and Noct/BeUI supports zedBSD upstream | WS006, WS007, WS008, WS017 |
| M6 — Accelerated graphics | `/dev/gpu0`, i915, the declared Vulkan profile, and GLES-on-Vulkan operate coherently | WS004, WS014, WS007 |
| M7 — Wayland desktop | A zedBSD Wayland compositor/DE runs on the supported input and graphics stacks | WS006, WS007, WS014 |
| M8 — CPAR environments | Boot environments are manageable as files, and selected services and runtime environments use immutable images and an honestly documented isolation profile without displacing traditional services | WS002, WS003, WS012, WS013 |
| M9 — Asymmetric hard real time | A declared μITRON-compatible profile runs resident work on statically reserved RT cores, communicates with POSIX through a bounded message bridge, and meets a published board-specific latency and limited failure-recovery contract | WS001, WS003, WS004, WS009, WS015 |
| M10 — Latitude NVMe overlay installation | The ordinary USB system installs into an existing ESP plus selected existing FAT32 without formatting; installed `BOOTX64.EFI` boots the NVMe-backed overlay root from the internal SN740 | WS003, WS004, WS009, WS013, WS019 |
| M11 — Latitude native installation | A later installer step creates or selects the required native filesystem and boots it with `rootpart=` without regressing M10 | WS003, WS004, WS009, WS013, WS019 |
| M12 — Panasonic CF-SV7 USB shell | The CF-SV7 clears its captured post-RSDP early interrupt stop, then reaches USB-backed init/login/shell with bounded diagnostics and final repeatability evidence | WS003, WS004, WS006, WS009 |
| M13 — Intel Mac UEFI boot | A fixed amd64 UEFI-only image with a pure Protective MBR and no BIOS path boots an Intel Mac to login, while generic Variant support preserves combined and BIOS-only images | WS003, WS009, WS013, WS020 |
| Continuous | POSIX debt and public documentation remain traceable | WS001, WS009, all producers |

## 5. Dependency map

```text
WS002 service baseline
  +-- WS005 networkd/net/WLAN expansion
  +-- WS011 net console + /etc/net.conf
  +-- WS012 service administration console -- WS013 service containers

WS011 configuration model
  +-- WS005 physical network/WLAN orchestration
  +-- WS011 VLAN/bridge data path (joint UAPI review with WS005)

WS003 hardware inventory
  +-- WS004 xHCI + USB storage -- WS003 QEMU/Latitude USB root
  |                                  +-- USB Ethernet -- WS003/WS005 network
  +-- WS003 p020 CF-SV7 early ACPI/IRQ -- p021 portable GPT image -- CF-SV7 USB root
  +-- WS004 PCIe/DMA/interrupts
       +-- NVMe
       +-- RTL8822BU USB WLAN -- WS005 wifi/networkd (first WLAN target)
       +-- RTL8822CE PCI WLAN (later target)
       +-- i915 prerequisites -- WS014 GPU/Vulkan/GLES -- WS007 Wayland

WS004 xHCI -- WS006 USB HID -- evdev
                                  +-- WS007 X11/Wayland
                                  +-- WS008 BeUI

WS003 p014/p015 signed swap sources + VM/VFS ownership
  +-- WS016 runtime swap manager -- /dev/system UAPI -- swapon/swapoff

WS007 Xzed + /dev/graphics + VM device mappings
  +-- WS017 optional LFB mmap -- Xzed fast path -- amd64 UEFI acceptance

WS003 boot/storage + WS006 evdev + WS007 Xzed/graphics + WS016 swap
  +-- WS018 source ownership and stable-interface consolidation
        +-- driver/platform/disk-label relocation
        +-- independent UFS/FAT and filesystem-owned probes
        +-- Xzed evdev-only -> independent mouse producers -> remove /dev/mouse
        +-- independent graphics frontends
        +-- boot/FAT native VFS consolidation -> remove bootfs

WS018 completion -- WS020 generic target Variant
                    +-- amd64 combined / UEFI-only / BIOS-only image profiles
                    +-- fixed pure-PMBR primary-only GPT -- Intel Mac UEFI bring-up

WS013 Boot foundation -- WS003 p011-p015 common x86 parameters
                       +-- UEFI same-disk FAT16/FAT32 `zedbsd.cfg`
                       +-- direct overlay/native parameters; no menu yet
                       +-- later PC/AT PBR/BOOTZBSD `/zedbsd.cfg`
                       +-- later PC-98 BOOTZBSD `BOOTZBSD.CFG`
WS013 Runtime CPAR -- WS012 service administration
                   +-- WS005 optional network profiles
                   +-- WS004 persistent image storage

WS004 p022-p025 NVMe driver/read-only hardware acceptance
  + WS013 p002/p003 same-disk zedbsd.cfg discovery + direct parameter translation
  + WS019 p002-p005 read-only diskpart + existing-FAT zedinst acceptance
       -> WS003 p018 Latitude existing-FAT overlay install and boot
       -> WS009 USB-trial/install/recovery guide
       -> later WS019 native/destructive Phases -> WS003 p019 native boot

WS004 SMP/IRQ/timer/reset + WS001 POSIX boundary
  +-- WS015 μITRON asymmetric RT domain -- WS009 public API and timing contract

WS001 compliance and WS009 documentation cross all workstreams.
WS010 supplies host-side build and test scripting used by all workstreams.
```

## 6. Priority waves

The user replaced the earlier wave ordering on 2026-08-30 with the following
active execution order. Dependency closure may interleave adjacent WSs, but it
must not silently promote a lower-priority product goal over a ready higher-
priority one.

1. Continue the WS006/WS004 completion audit. q044 completed WS006 p006/p007
   source boundaries without calling the WS complete; p008/p009 remain. WS004
   likewise retains its independently listed p017, p019, p021, p025, and WLAN
   chain rather than treating the working visible USB/NVMe cases as closure.
2. WS018 is complete.  q035 finished p009's retained runtime evidence and
   p010--p012 in consumer-before-deletion order; no residual Phase remains.
3. Complete WS020 Intel Mac bring-up. Add the generic board Variant, implement
   combined/UEFI-only/BIOS-only amd64 image profiles, pass the six-cell QEMU
   matrix, then perform the bounded physical UEFI-only checkpoint and final
   five-run acceptance. Disk capacity is not a menu selection.
4. Implement the Archer T3U Nano USB WLAN path across WS004 and WS005. The
   dependency order is p026 exact-unit/firmware intake, p027 generic WLAN core,
   WS005 privilege/command/profile/protocol prerequisites, p028--p030 radio and
   WPA2 lifecycle, then WS005 orchestration and the single shared hardware
   acceptance. The SSH host `awe@10.0.10.25` may be used for QEMU USB
   passthrough of the connected adapter after automatic gates pass.
5. Complete the `/sbin/net` WLAN stack in WS005 through the fixed
   `net` -> `networkd` -> `ifconfig`/`wifi`/`dhcpc` topology.
6. Resume WS008 from the maintainer's latest accepted NoctLang tree. First
   restore and verify the host `--path`/`require` CLI contract in p010; then
   update the userland package through p009. The latter is rooted at
   `userland/base/noct/` and clones upstream into
   `userland/base/noct/noct/`.
7. Implement WS017's `/dev/graphics` LFB fast path after resolving its retained
   `mprotect` permission-ceiling decision. Do not infer that decision from the
   priority change.
8. Retain q043's `ws007-p004` result as `uncleared`: the frozen headless PC-98
   cell and focused input gates pass, while the maintained qemu-pc98 build has
   no interactive display backend. Resume only from the user's exact failing
   GUI environment. After WS017, continue the remaining WS007 integration,
   including real Xzed/LFB behavior.
9. Continue WS001 POSIX work. Add a bounded `lp`/`lpr` Phase whose deliberate
   model posts PDF directly to an LPD printer and has no local spool queue.
10. Re-plan WS019 so the installer is written in Noct, then implement it after
   its complete installer specification is supplied. The user's latest message
   ended after "仕様は"; that missing contract is a human blocker and must not be
   guessed from the older C-oriented plan.
11. After the installer passes automatic acceptance, complete WS003 by
    installing to and booting from the Latitude 5320 NVMe device.
12. Resume WS011 VLAN and bridge work. The previous manual hold is released by
    this priority instruction, but any still-open virtual-interface, packet-
    ownership, filtering, or persistence decision remains a design gate rather
    than permission to improvise an incompatible UAPI.
13. Keep WS013 Runtime CPAR, WS014 GPU, and WS015 μITRON pending until their
    explicit architecture holds are separately released.

When a higher-priority item reaches a recorded human decision, mark only that
Phase `uncleared`/deferred and continue to the next dependency-ready item. A
Queue remains finite even though the user authorized repeated Queue creation;
on finishing one Queue, construct the next from the order above and continue
until stopped or no judgment-free Phase remains.

## 7. Decisions that gate new Phases

| Decision | Owning WS | Required before |
| --- | --- | --- |
| Exact Latitude BIOS, boot mode, PCI/USB topology and IDs | WS003 | Driver selection and hardware acceptance |
| Exact CF-SV7 DMI identity, firmware settings, CPU/APIC mode, PCI/USB topology, and IDs | WS003 | Later device-specific driver selection; p020 early IRQ and p021 portable-GPT work do not depend on the remaining inventory |
| Intel Mac identity, firmware, and target-medium inventory | WS020 | p004 physical acceptance only; no exact capacity match is required because the fixed GPT extent may precede the physical end |
| UEFI-only Protective MBR and primary-only GPT shape | WS020 | Resolved: zero non-executable Protective-MBR code, one non-active `0xee` entry, three zero entries, and `55 aa`; the fixed 395,297-sector artifact contains a valid primary GPT and zero final 33-sector reservation but no backup GPT or compatibility-MBR entry |
| Initial Secure Boot scope | WS003 | Resolved: use UEFI with Secure Boot disabled; signing/key enrollment deferred and not required for NVMe |
| USB Ethernet interface descriptors and, for vendor-specific interfaces, VID:PID/controller family | WS003/WS004/WS005 | Choose CDC ECM/NCM class frontend or Realtek-family frontend for HW-12/NET-10; ACM is inapplicable |
| First USB WLAN identity | WS004/WS005 | Primary evidence identifies Archer T3U Nano V1.0 as RTL8822BU, USB `2357:012e`; `ws004-p026` must still record the exact physical unit revision, descriptors, and firmware provenance before driver implementation |
| Built-in PCI WLAN identity | WS004/WS005 | Resolved as RTL8822CE `10ec:c822`, subsystem `10ec:c130`; retained as a later target after the Archer-first sequence |
| WLAN privilege and process topology | WS005 | Resolved for v1: one `root:network` mode-0660 `/run/networkd.sock`, kernel-attested connection-time peer credentials, `net` as user/desktop frontend, root `networkd` as orchestrator, and fixed primitive `ifconfig`/`wifi`/`dhcpc` children; no resident/pluggable `wpa` child |
| WLAN protocol-state ownership | WS004/WS005 | Resolved for v1: a device-independent kernel WLAN layer retains scan/authentication/association/WPA2 key and rekey/controlled-port state after one-shot `/sbin/wifi` exits; the RTL8822BU driver owns only hardware/firmware-specific radio, USB, frame, channel, and key-slot operations |
| `/etc/net.conf` v1 grammar and empty-collection syntax | WS011 | Parser and boot migration |
| VLAN/bridge virtual-interface UAPI and packet ownership | WS005/WS011 | Manual hold released 2026-08-30; `ws011-p004` must now close the remaining design gates before implementation |
| Linux/FreeBSD evdev compatibility profile | WS006 | Resolved by `ws006-p001`; implement `/dev/input/eventN` against it |
| Device-mapping `mprotect` ceiling | WS017 | Choose whether an initial RW mapping may return from RO to RW within its original maximum, or whether every permission reduction is permanent, before `ws017-p001` enters a Queue |
| zedBSD GPU/Vulkan capability, object, and display-takeover profile | WS014 | Manually blocked; publishing `/dev/gpuN` UAPI or transferring i915 ownership |
| YAML `/etc/rc.conf` schema and versioned init status/control protocol | WS012 | Resolved and complete: q017 completed YAML/persistence; q018 completed typed `/run/init.sock` service and `ZSV1 HALT`/`POWEROFF`/`REBOOT` clients, argv/interactive administration, and production integration with no unversioned compatibility path |
| x86 kernel boot-parameter contract | WS003/WS013 | Resolved and implemented by q015: `boot0`--`boot3`, exclusive `rootpart` or explicit overlay root/data, `swap0`--`swap3`, and `init`; BR-T46 passes all 31 four-platform QEMU cells |
| UEFI `zedbsd.cfg` boot contract | WS013 | Resolved for q031: required same-disk FAT16/FAT32 `/zedbsd.cfg`, required loader-only `kernel=`, direct kernel parameters with bounded shorthand, overlay or native `rootpart`, no menu, and ignored UEFI LoadOptions |
| BIOS boot configuration names and convergence | WS013 p005/p006 | Resolved and implemented by q032: i386 PC/AT PBR/`BOOTZBSD.EXE` and amd64 BIOS PBR/`BOOTZBSD.EXE` use `/zedbsd.cfg`; PC-98 `BOOTZBSD.EXE` uses `/BOOTZBSD.CFG`; all implement the p003 format and parameter result with no `boot.cfg`, fixed-kernel, embedded-record, or reserved-area direct-kernel fallback |
| NVMe partition naming | WS004/WS019 | Resolved by the existing one-based disk contract: namespace is `/dev/nvme0n1`, first partition is `p1`; the earlier `p0` example is not a new ABI |
| Installer v1 layout and mutation boundary | WS019 p001 | Resolved: existing GPT, exactly one existing ESP, one explicitly selected distinct same-disk FAT32, no mkfs/GPT writes/label writes, overlay files only, and no firmware-variable mutation |
| Installer read-only administration UAPI | WS019 p002 | Expose stable whole/partition identity, GPT type/PARTUUID, parent relation, capacity, filesystem type, mount/swap state, and loader-origin identity before read-only `diskpart` and `zedinst` preflight |
| Installer payload discovery | WS013 p002 | Resolved for q031: search same-physical-disk FAT16/FAT32; zero `/zedbsd.cfg` candidates is fatal, multiple candidates warn and use the deterministic first, and omitted `boot0` defaults to the selected config FAT while an explicit value is preserved |
| Installed UEFI `LoadOptions` precedence | WS013 p002/p003 | Resolved for q031: ignore LoadOptions on the required `zedbsd.cfg` path; do not merge or override the configuration |
| Installer source-image stability | WS019 p004 | Choose unused immutable installer templates for data/swap (recommended) or explicitly redesign p004 to generate them at the target; never copy the live overlay upper or active swap |
| Runtime CPAR namespace/security, CLI/build, and service-package contracts | WS013 | Manually blocked; any Runtime CPAR implementation Phase |
| Confirmed-commit implementation bounds | WS011 | Public semantics are fixed: interactive only, explicit timeout, delayed `/etc/net.conf` write, ordinary `commit` confirms, and DHCP is reacquired; freeze timeout maximum, lock path, and diagnostic bounds before implementation |
| Authoritative Noct repository, build sequence, and revision | WS008 | Official main remains `awemorris/NoctLang`. q041 pinned host commit `3bf3d236...`, but p010 records that it rejects the still-documented `--path`/`require` CLI used by production scripts; the accepted replacement revision is manually blocked by `MB-008`. Target relocation remains p009. |
| PC/AT boot selector | WS004 | Resolved: reuse UUID/PARTUUID; standard FAT handoff uses UUID |
| Runtime swap command standard and control boundary | WS016 | Resolved for v1: SUSv4/POSIX does not define `swapon`/`swapoff`; zedBSD supplies minimal privileged extensions over versioned `/dev/system` control and existing signed sources |
| Optional LFB mapping and Xzed fallback boundary | WS017 | Resolved for v1: fixed post-ENTER geometry, 8/16/24/32-bpp layout query, shared non-executable mmap when supported, true-color Xzed fast path, and unchanged ioctl fallback; PC-98 Cirrus is excluded |
| μITRON compatibility and RT isolation contract | WS015 | Manually blocked by `MB-007`; user-mode resident ELF and explicit MMIO grants are fixed, but exact profile, legacy static configuration, RT CPU/IRQ/timer model, RT/POSIX mailbox/filesystem ownership, limited POSIX-failure recovery, and first board-specific latency target remain required before any implementation Phase |

### 7.1 Manual blocking register

These are explicit user holds, not missing implementation authorization. They
remain visible but are excluded from Queue proposals and active design agendas
until the user explicitly releases the named hold.

| Hold ID | Owning WS/Phase | Manually blocked topic | Resume condition |
| --- | --- | --- | --- |
| `MB-002` | `ws013-p001` | Runtime CPAR namespace, isolation, and security model | User explicitly resumes Runtime CPAR namespace discussion |
| `MB-003` | `ws013-p001` | `cpar run`, `cpar sh`, and `cpar build` grammar/lifecycle | User explicitly resumes Runtime CPAR CLI/build discussion |
| `MB-004` | `ws013-p001` | Service-container package format, dependencies, updates, config, and data | User explicitly resumes service-container package discussion |
| `MB-005` | `ws014-p001` | GPU UAPI, capability profiles, display takeover, i915 split, Vulkan/GLES | User explicitly resumes GPU architecture discussion |
| `MB-007` | `ws015-p001` | μITRON profile/UAPI, legacy static configuration, RT/POSIX mailbox and filesystem proxy, scheduling, failure, and timing contracts | User explicitly resumes WS015 architecture/API discussion |
| `MB-008` | `ws008-p010` | Noct host CLI repair ownership and replacement revision after pinned `3bf3d236...` removed the documented `--path`/`require` contract | Maintainer publishes and identifies an accepted upstream repair, or the user explicitly selects a compatible older immutable revision |

Released holds remain permanent history rather than reusable identifiers:

| Hold ID | Released | Result |
| --- | --- | --- |
| `MB-001` | 2026-08-30 | The user placed VLAN and bridge implementation back in the active priority order. The manual hold is removed; unresolved virtual-interface UAPI, packet ownership, filtering, and persistence details remain ordinary p004 design gates. |
| `MB-006` | 2026-08-30 | The user resumed WLAN design after completing USB Ethernet. The old RTL8822CE-first, `/sbin/wpa`, and `/etc/wpa/` proposal was superseded by the Archer T3U Nano first target and the fixed `net` -> `networkd` -> `ifconfig`/`wifi`/`dhcpc` topology. Firmware and exact-device facts are ordinary Phase dependencies, not a continuing manual hold. |

## 8. Interruption and resumption

A WS does not need to complete all planned Phases in one execution period. A WS
may be paused after a Phase, or a Phase may be interrupted at a safe checkpoint.
At interruption:

1. this master records the WS status, last verified Phase, and exact resume
   point;
2. `ws.md` records completed, current, and remaining Phase rows;
3. the active `phase.md` records completed work packages, failing/unrun tests,
   working-tree assumptions, and the next safe action;
4. unresolved POSIX findings are also entered in WS001;
5. partial or stub behavior is never reported as completed acceptance.

The detailed state vocabulary and file contracts are in
[governance.md](governance.md).

## 9. Reconsideration boundaries

Stop the active Phase and update its state before changing the plan when:

- target hardware IDs do not match the selected driver family;
- a stable UAPI cannot represent the target without a breaking redesign;
- required firmware has unresolved loading, licensing, or redistribution
  constraints;
- QEMU cannot model the required hardware and no safe test double, passthrough,
  or physical diagnostic path exists;
- USB root requires a boot/root architecture change outside the active Phase;
- the Latitude firmware cannot boot the deliberately selected GPT/ESP layout,
  or safe NVMe installation requires preserving/resizing an
  unknown existing filesystem;
- a requested compatibility target conflicts with an explicit zedBSD design
  policy.
- a hard-real-time or POSIX-crash-survival claim cannot be supported by the
  selected core, interrupt, firmware, memory, or shared-kernel isolation model.

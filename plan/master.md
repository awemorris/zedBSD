# zedBSD master plan

Last updated: 2026-09-03

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
strict/sanitizer/analyzer checks. Q047 completed p008's USB 1.1 concurrency,
hotplug, and checked recovery prerequisites. Q048 then completed the production
Report-Protocol keyboard/mouse/tablet automatic milestone, including dynamic
generation-safe evdev publication and xHCI plus paired EHCI/UHCI QEMU runtime;
one bounded physical HID observation remains. Q063 now accepts official Noct
`v2.0.1` for both host and target: runtime and compile/application `--path`,
clean/incremental toolchain, ordinary production build, amd64 static/package
identity, non-JIT, RW-to-RX/JIT, and canonical BeUI q35/xHCI gates pass. Its
tracked two-hunk target patch connects only the zedBSD final-link adapter and
is explicitly not a BeUI adapter.

Q064 completes the reproducible x86 compiler boundary. Verified patched LLVM
23.1.0, its permanent digest-pinned `rev-0` x86_64 Linux cache, amd64/i386
sysroots, all maintained x86 kernel/userland/loaders, and all four CI image
configurations pass. The consolidated runtime campaign covers every amd64
firmware/Variant cell, i386 PC/AT, maintained PC-98, and target noct non-JIT,
JIT/RW-to-RX, and BeUI on amd64 UEFI. WS022 records the separately deferred
compiler-emitted static ELF `PT_TLS` work.

WS023 records the completed x86 HAL readability correction. Its q067 audit
refactored all 88 maintained i386/amd64 HAL C/header files, including the five
inherited i386 edits from `b4be6eb`, through small behavior-preserving reviews.
Strict/focused tests, all four configured image builds, and PC/AT, PC-98,
amd64 BIOS SMP, and amd64 UEFI SMP runtime cells pass without a public HAL API,
ABI, or intended hardware-policy change.

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
records the then-uncleared printed-label-only Archer identity checkpoint and the
completed independent AF_UNIX/network authorization foundation. The archived
[q044](queue-q044.md) records the completed input ownership and bounded HID
parser milestones. The archived [q045](queue-q045.md) records the independent
WS004 SuperSpeed interrupt-context automatic/source correction. The completed
[q046](queue-q046.md) reconciles the producer-linked evdev/input and kernel
boot-parameter references. The archived [q047](queue-q047.md) completed the
WS004 legacy-HCD concurrency/hotplug and checked USB recovery prerequisites
before USB HID. It completed p031's focused, configured-production,
regression, repository-build, and forced standalone/paired QEMU gates, p032's
checked USB recovery and reclaim-safe reserve boundary, and p033, the bounded
amd64 framebuffer-console race extracted when p031's third hot-add exposed a
cursor-row overwrite. No physical recovery result is claimed for p032. The
archived [q048](queue-q048.md) completes the WS006 p008 automatic HID milestone
with xHCI and paired EHCI/UHCI acceptance; it does not claim the remaining
physical IN-T42 observation. Completed [q049](queue-q049.md) contains WS004
p019: the independent standards CDC ECM driver, general zero-packet HCD
contract, and passing four-cell IDE/xHCI-storage static/DHCP QEMU baseline.
Completed [q050](queue-q050.md) closes the two VFS prerequisites under their
canonical IDs `ws001-p022` and `ws001-p023`. Production-linked rollback and
synchronization gates plus five bounded abrupt-stop/remount launches pass.
The completed [q051](queue-q051.md) closes `ws005-p005`: the real root/non-root
`/sbin/net wifi set-key`, read-side inode replacement rejection, and
abrupt-stop/reboot persistence boundaries pass. Completed [q052](queue-q052.md)
then consumed q047's repaired Noct verifier boundary for `ws004-p021`. All
current-source xHCI/NCM gates, a
fresh private configured build, and one OVMF q35/xHCI USB-root boot pass. One
hash-pinned Latitude/RTL8156 observation remains, so the Phase is honestly
`uncleared` while q052 itself is finished. The archived
[q053](queue-q053.md) then completed the documentation-only `ws005-p002`
closure: the frozen WLAN v1 topology, ownership, security and supersession
records agree with all dependent P-books, including exact 15-second scan and
30-second direct-connect budgets. No source, radio, or hardware result is
attributed to that design Phase. The archived [q054](queue-q054.md) completed
`ws004-p017`: accepted NCM packets/bytes remain counted, later
`STALL`, `TIMEOUT`, `DISCONNECTED`, or `IO_ERROR` adds exactly one locked TX
error without a drop, and administrative `CANCELLED` adds none. Focused,
sanitizer, analyzer, retained-regression, configured x86 build, and fresh OVMF
q35/xHCI USB-root exact-login gates pass. Q055 then closed p026 with the exact
Japan-unit descriptor and explicit optional-package decision and completed
p027's generic WLAN UAPI/common core before any Realtek register access. Q056
completed p036's independently testable RTL8822BU pre-radio substrate and
per-device firmware hierarchy. Q057 completed p028's automatic WLAN milestone:
the notice-preserving BSD-3-Clause tables and binary license, bounded RTL8822B
radio programming, conservative ch1--11 scan, failure rollback, and focused/
build/IDE/xHCI gates pass without claiming physical RF. Q058 completed
p029's strict WPA2-Personal/CCMP handshake, controlled port, RTL keys, and
synthetic Ethernet L2 automatic milestone. Q059 completed WS005 p004's
minimum direct `/sbin/wifi` command and p009's single USB-passthrough
development checkpoint: scan, WPA2/CCMP authorization, DHCP, gateway/public
ping, bounded HTTP fetch, disconnect, and interface down passed. Q060 completed
WS004 p030's automatic RTL8822BU lifecycle milestone, while its shared WS005
p008 physical lifecycle and five-run closure remain pending. Q061 completed
p037's read-only Intel intake, corrected the exact target to AX211/CNVio2
`8086:51f0`, subsystem `8086:4090`, revision `01`, and cleared its firmware/
license and direct-boot boundaries. Q062 completed p038's initial implementation
and focused automatic package/PCI/MSI-X/DMA/firmware/runtime/scan/security/L2/
lifetime gates. Q066 completed the API89 MLD correction and exact-device VFIO
normal path through WPA2/CCMP, DHCP, ping, nonempty HTTP fetch, disconnect,
down, and host restoration; p038 is uncleared only for one exact AX211 direct-
boot result shared with the HAL counter observation. P039's evidence-
driven cross-driver review follows that physical checkpoint before the return
to p006/p007. P008 final five-run acceptance remains incomplete. Q063
independently closes the former Noct compile/application CLI defect and target
package hold. The
archived q043 PC-98 fixed-read and exact-mouse
paths both pass locally, and their remaining external resume facts do not
block subsequent Queues. The
archived [q038](queue-q038.md) replaced the
stale or intermediate image used for the first Intel Mac observation with one
fresh, production-checked UEFI-only artifact and passed the exact larger-USB
path through init/login. Q047 refreshed `MAC-T021`, the partition-publication
ordinary/sanitizer/analyzer gates, and the installed handoff bytes at SHA-256
`f811a0f5...`. The first provisional Intel Mac boot of those exact bytes
reached the kernel, USB storage, and current boot parameters, then rejected a
host-relocated physical-end GPT because its Protective MBR still advertised the
compact source extent. Q047 therefore extracted `ws020-p006`: it makes a
CRC-valid, structurally valid GPT authoritative over the Protective MBR sector
count while preserving strict bounds, copy-consistency, and corruption
checks. Its automatic gates precede one refreshed
provisional boot; p004 still ends with the final five cold boots.
The archived [q037](queue-q037.md) passed every automatic native PC-98 IPL and
INT 1Bh disk-read gate while retaining the native partition layout and `55 aa`;
its frozen image still awaits the already-requested PC-9821V13 observation.
The archived [q036](queue-q036.md) completed the generic Variant and three
fixed amd64 image-layout Phases through their automatic layout gates. Its
strict six-cell runtime Phase was uncleared because three fresh runs exposed an
image/firmware-independent init/getty scheduling flake after root and swap had
already succeeded. Q047 later completed p003 with one uninterrupted six-cell
PASS using the original exact `login:` and negative-cell oracles.
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
Phase were excluded from q029. The former is complete as `ws004-p019` in
q049; the latter remains independent future work.

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
implementation quality. At that historical point Noct entered maintainer-only
manual repair; q063 later consumes the accepted official release. The
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

The bootstrap needed to obtain Noct may use Make, the host compiler, CMake, a
downloader, checksum/archive tools, and a minimal shell recipe because Noct
does not exist yet at that point. Q063 obtains the verified official release
archive rather than cloning a Git checkout.
Ordinary Make recipes that directly invoke tools are not required to be
rewritten as scripts. When a value can be expressed clearly as maintained
source or an ordinary Make dependency, prefer that over generating source at
build time; Noct is the implementation language when a project-owned script is
actually warranted.

WS021 extends this bootstrap without changing its ownership boundary. The host
C/C++ toolchain builds host Noct and the project-owned LLVM 23.1.0 installation;
then only `build/llvm/` builds supported i386/amd64 zedBSD kernels, userland,
target Noct and bootloaders. Host Noct is not rebuilt as a zedBSD target merely
because it orchestrates those target builds. The x86 sysroots are
`build/amd64/sysroot` and the PC/AT/PC-98 shared `build/i386/sysroot`.

For new hardware and protocol bring-up, establish one bounded, useful normal
path before perfecting every abnormal and semi-normal branch. The first slice
still requires essential bounds, finite waits, checked returns, and secret
redaction. Exhaustive cancellation, recovery, race, fault-injection, hotplug,
and repeatability matrices are retained as explicit later Phases and are not
allowed to block first communication unless the normal path depends on them.

## 3. Workstream registry

| WSID | Workstream | Status | Last completed / current Phase | Resume point | WS plan |
| --- | --- | --- | --- | --- | --- |
| `ws001` | POSIX.1-2024 compliance | Active ledger; q050 completed both VFS prerequisites discovered by q041 | `ws001-p022` and `ws001-p023` complete with production-linked faults and abrupt-stop/remount evidence | Retain p022/p023 as regressions; their dependency consumer resumes in WS005 p005 | [WS001](ws001-posix/ws.md) |
| `ws002` | System services | Complete baseline; p022 corrective complete | `ws002-p022` complete; USB submit-commit local-IRQ self-wait repaired and five final exact-login boots pass | Retain the p022 regression; p021 remains separately planned and non-blocking | [WS002](ws002-services/ws.md) |
| `ws003` | x86 laptop and PC-98 hardware bring-up | Active; q066 completed p025's automatic HAL counter milestone while the completed Latitude USB/network and CF-SV7 USB-root milestones remain intact | p025's API split, private calibration, complete-CPU-set validation, positive/negative SMP KVM evidence, and configured build matrix pass; p024 still awaits one V13 boot and p018/p019 remain | Share p025's sole remaining physical multicore observation with p038's final direct boot rather than creating an intermediate human block | [WS003](ws003-bringup/ws.md) |
| `ws004` | Hardware expansion | Active; q066 completed the AX211 API89 correction and exact-device VFIO useful normal path with checked host restoration | `ws004-p010`--`p020`, p022--p024, p026-p030 automatic, p031--p033, p036, and p037 are complete; p038's automatic and VFIO milestones pass | Use one direct boot to close both p025 physical-counter and p038 native-network evidence, then perform p039's evidence-driven cross-driver review | [WS004](ws004-hardware/ws.md) |
| `ws005` | Networking and WLAN | Active; q059 completed p004 minimum direct command and p009 one-run physical connectivity; q060 completed the p030 automatic dependency | Physical USB Ethernet, authenticated control, strict root/per-user credential storage, frozen design contract, exact first-radio identity, common WLAN control ABI, p036, p028-p030 automatic milestones, p004, and p009 are complete; q066 completed WS004 p038's exact-device VFIO normal path, while its native direct boot remains | After WS004 p038/p039, complete p006/p007/p010 before the still-pending shared p008 final acceptance | [WS005](ws005-networking/ws.md) |
| `ws006` | Input and evdev | Active; q048 completed the production Report-Protocol HID automatic/software milestone with generation-safe stale-fd handling and xHCI plus paired EHCI/UHCI runtime | `ws006-p008` automatic/software milestone complete; IN-T42 physical observation and p009 remain | Record one bounded physical keyboard/mouse observation; q063 released p009's WS008 userland dependency | [WS006](ws006-input/ws.md) |
| `ws007` | Graphics and desktop | Active; q039 PC-98 cascade repair complete, q043 local exact-reproduction matrix passes while the newer GUI report remains external | `ws007-p001` and `p003` complete; p004 uncleared; amd64 `p002` carried | Resume p004 only from the user's exact failing image/QEMU/interactive-backend/focus record; do not alter the passing headless path speculatively | [WS007](ws007-graphics/ws.md) |
| `ws008` | Noct and BeUI | Complete (`q063`) | p010 host CLI/toolchain/ordinary-build and p009 amd64 static/package/q35-xHCI runtime gates pass on official `v2.0.1`; p006 remains historical review evidence | No current Phase; Remacs and i386/PC-98 target Noct remain explicitly outside q063 | [WS008](ws008-noct/ws.md) |
| `ws009` | Documentation | Active; q046 reconciled two already implemented producer contracts | `ws009-p004` and `p005` complete | Extract the next dependency-ready public reference alongside its producer WS | [WS009](ws009-documentation/ws.md) |
| `ws010` | Noct scripting and x86 image tools | Complete (`q063`) | `ws010-p001`–`p005` complete; all 177 maintained userland Makefiles expose the common lifecycle and top-level download materializes declared external inputs | No current Phase; extract a new requirement before resuming | [WS010](ws010-scripting/ws.md) |
| `ws011` | Network configuration console | In progress; VLAN/bridge hold released, detailed design still open | `ws011-p003` complete; p004 resumed for design; p005 bounds open | After higher priorities, finish p004's virtual-interface/UAPI/packet-ownership design before implementation; freeze p005 bounds independently | [WS011](ws011-net-config/ws.md) |
| `ws012` | Service administration console | Complete (`q018`) | `ws012-p006` complete | No current Phase; extract a new requirement or continue container integration in WS013 | [WS012](ws012-service-console/ws.md) |
| `ws013` | CPAR container partitioning | Active; q031/q032 configured x86 boot paths complete, Runtime topics manually blocked | `ws013-p002`--`p006` complete | No Boot configuration Phase remains; resume Runtime namespace/CLI/package design only after its explicit manual holds are released | [WS013](ws013-containers/ws.md) |
| `ws014` | Native GPU stack | Blocked by manual hold | `ws014-p001` is blocked before detailed design | Resume only after explicit user release | [WS014](ws014-gpu/ws.md) |
| `ws015` | μITRON asymmetric real-time domain | Blocked by manual hold `MB-007`; user-mode RT direction recorded | `ws015-p001` is the only current Phase | After explicit hold release, select the μITRON profile and freeze the remaining RT/POSIX, mailbox/filesystem, failure, and timing contracts | [WS015](ws015-muitron-rt/ws.md) |
| `ws016` | Runtime swap control | Complete (`q021`) | `ws016-p004` complete; SWAP-T001--T012 and the six-cell amd64 UEFI matrix pass | No Phase remains; extract a new requirement before resuming | [WS016](ws016-swap-control/ws.md) |
| `ws017` | `/dev/graphics` LFB fast path | Planned; p001 blocked on one human `mprotect` decision | No Phase started | Choose the mapping permission ceiling, then Queue p001 device-mmap/UAPI followed by p002--p004 | [WS017](ws017-lfb-graphics/ws.md) |
| `ws018` | Kernel source ownership and interface consolidation | Complete (`q035`) | `ws018-p012` complete; p001--p012 all cleared | No Phase remains; extract a new requirement before resuming | [WS018](ws018-kernel-architecture/ws.md) |
| `ws019` | Installation and disk administration | Re-plan required; installer language changed to Noct | `ws019-p001` retains the approved storage safety contract; older p002--p005 implementation language is superseded pending revision | Do not implement from the old C-oriented Phase map. The latest request ended after `仕様は`; obtain the missing Noct installer contract, then rewrite the bounded implementation/acceptance Phases | [WS019](ws019-installation/ws.md) |
| `ws020` | Intel Mac UEFI bring-up and generic image variants | Active; p006 automatic GPT/Protective-MBR repair complete, one provisional and p004 final physical acceptance pending | `MAC-T022`, pristine `MAC-T021`, and uninterrupted six-cell `MAC-T020` pass; exact `692160cf...331d` / `A93F-BBBE` image published | Record one provisional Intel Mac boot, then retain p004's final five consecutive cold boots | [WS020](ws020-intel-mac/ws.md) |
| `ws021` | Reproducible x86 LLVM toolchain and sysroots | Complete (`q064`) | LLVM 23.1.0 cache/source paths, amd64/i386 sysroots, all x86 target/loader builds, four CI configurations, six-cell amd64 firmware matrix, i386 PC/AT and PC-98, and target noct non-JIT/JIT/BeUI gates pass | No current Phase; the source-build path and pinned `rev-0` cache remain supported in parallel | [WS021](ws021-llvm-toolchain/ws.md) |
| `ws022` | ELF `PT_TLS` and static thread-local storage | Planned; WS021 dependency satisfied, not queued | No Phase started | Freeze the x86 TLS/TCB ABI and fixture matrix in p001 before changing exec or pthread runtime | [WS022](ws022-elf-tls/ws.md) |
| `ws023` | i386/amd64 HAL coding-style conformance | Complete (`q067`) | All 88 C/header files, focused/strict gates, four configured builds, and four x86 runtime cells pass; API/ABI review found no delta | No current Phase; retain the q067 evidence and extract pre-existing risks separately if prioritized | [WS023](ws023-x86-hal-style/ws.md) |

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
| M14 — Reproducible x86 toolchain | Host tools bootstrap LLVM 23.1.0 below `build/llvm`; every amd64/i386 kernel, userland, target Noct and BIOS/UEFI loader uses the project toolchain/sysroots and passes one final six-cell QEMU campaign | WS008, WS010, WS021 |
| Continuous | POSIX debt, public documentation, and maintainable source form remain traceable | WS001, WS009, WS023, all producers |

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
       +-- Intel AX211/CNVio2 identity/firmware intake
       |    +-- standalone Intel AX211 normal path
       |         +-- post-success RTL/Intel commonization review
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
WS010 host Noct + host C/C++ bootstrap -- WS021 LLVM 23.1.0
  +-- build/amd64/sysroot + build/i386/sysroot
  +-- amd64/i386 kernels and userland -- WS008 target Noct
  +-- BIOS/UEFI loaders and all x86 images -- final big-bang QEMU matrix

WS003/WS004 x86 HAL behavior + WS021 project LLVM
  +-- WS023 i386-first, then amd64 source-style conformance
       +-- unchanged HAL API/ABI and hardware behavior
       +-- four configured x86 image builds and final QEMU regression
```

## 6. Priority waves

The user replaced the earlier wave ordering on 2026-08-30 and inserted the
approved WS021 LLVM toolchain plan on 2026-09-02. The following is the active
execution order. Dependency closure may interleave adjacent WSs, but it must
not silently promote a lower-priority product goal over a ready higher-priority
one.

The temporary WS023 maintenance override completed in q067. Product work may
therefore resume from item 9 below without carrying an active style Queue.

1. q048 completed the WS006 p008 production HID automatic/software milestone
   after q044's p006/p007 source boundaries and q047's p031/p032 USB 1.1,
   hotplug, and recovery prerequisites. Checked Report Protocol,
   generation-safe stale-fd handling, xHCI, and paired EHCI/UHCI QEMU runtime
   pass. One bounded physical HID observation remains without calling the WS
   complete. q049 independently implemented CDC ECM and proved its four QEMU
   network cells without making NCM wire behavior a shared implementation
   dependency. q047 also completed p033's framebuffer-console race correction
   using host gates and p031's passing forced QEMU evidence rather than adding
   a second run. P009 remains WS008-dependent. WS004 completed p019 in q049;
   q052 then completed p021's current-source automatic/runtime boundary and
   froze one candidate for its remaining Latitude check. Q053 subsequently
   completed the documentation-only `ws005-p002` WLAN v1 contract audit, and
   q054 implemented the accepted common asynchronous-TX statistics policy in
   NCM and completed p017. WS004 retains the p021 physical checkpoint, p025,
   and the WLAN chain rather than treating the working visible USB/NVMe cases
   as closure. ECM adoption of the helper is a separate future consumer, not a
   residual failure of p017.
2. WS018 is complete.  q035 finished p009's retained runtime evidence and
   p010--p012 in consumer-before-deletion order; no residual Phase remains.
3. Complete WS020 Intel Mac bring-up. The generic board Variant, three amd64
   image profiles, strict six-cell QEMU matrix, and refreshed production
   handoff preflight pass. The bounded physical UEFI-only checkpoint and final
   five-run acceptance remain deferred by the user. Disk capacity is not a
   menu selection.
4. Implement the Archer T3U Nano USB WLAN path across WS004 and WS005. The
   dependency order is p026 exact-unit/firmware intake, p027 generic WLAN core,
   the completed p036 pre-radio substrate with its individually selected
   firmware package, WS005 privilege/command/profile/protocol prerequisites,
   completed q057 p028 scan, q058 p029 secure L2, q059 p004 minimum command
   plus p009 single physical communication path, and q060 p030 automatic
   lifecycle hardening. P030's shared p008 physical/five-run acceptance remains
   pending and was not consumed by q060.
5. Execute the corrected Intel AX211/CNVio2 chain in strict order: q061
   completed p037's read-only exact-device and firmware/license intake. Q062
   completed p038's standalone exact `8086:51f0`/`8086:4090`/rev `01`
   implementation and focused automatic gates. Q066 completed its API89 MLD
   correction and exact-device VFIO normal path through firmware/PNVM, scan,
   WPA2/CCMP, DHCP, ping, nonempty fetch, disconnect, and down with host
   restoration; one exact-machine direct boot remains before p038 closes.
   Then p039 reviews/refactors only common behavior
   demonstrated by both working drivers. Do not create a common Intel/Realtek
   hardware layer first, and do not change the public WLAN UAPI without a
   significant explicit interface decision. RTL8822CE remains a later
   independent target.
6. Complete the `/sbin/net` WLAN stack in WS005 through the fixed
   `net` -> `networkd` -> `ifconfig`/`wifi`/`dhcpc` topology.
   Execute p006 then p007 composition, followed by ready p010 primitive
   hardening and the single final p008 hardware acceptance. P030's automatic
   dependency is complete; the shared physical subrecord closes inside that
   later p008 acceptance.
7. WS008 and WS010 are complete through q063. Official Noct `v2.0.1` is
   verified once and extracted for both host and target; host runtime and
   compile/application `--path`, ordinary build, amd64 target package, and
   q35/xHCI non-JIT/JIT/BeUI gates pass. The target package is rooted at
   `userland/base/noct/`, the tracked two-hunk patch connects only the zedBSD
   final-link adapter, and Remacs plus i386/PC-98 target Noct remain outside
   this completed item. All maintained userland items expose
   download/patch/build/install, with top-level `make download` providing the
   source-distribution acquisition boundary.
8. Retain q064's completed WS021 boundary: host Noct and verified patched LLVM
   23.1.0 bootstrap with the host toolchain; `build/llvm`, both x86 sysroots,
   every x86 target/loader, the permanent `rev-0` cache, CI builds, and the
   consolidated QEMU/target-noct acceptance all pass. Future x86 target work
   must keep using this project-owned toolchain boundary.
9. After WS021, implement WS022's compiler-emitted static ELF `PT_TLS`
   contract. Freeze the amd64/i386 TLS/TCB ABI and malformed-ELF fixtures
   before changing exec; then provide independent initial-thread and pthread
   TLS images. Dynamic `dlopen()` TLS remains outside the first boundary.
10. Implement WS017's `/dev/graphics` LFB fast path after resolving its retained
   `mprotect` permission-ceiling decision. Do not infer that decision from the
   priority change.
11. Retain q043's `ws007-p004` result as `uncleared`: the frozen headless PC-98
   cell and focused input gates pass, while the maintained qemu-pc98 build has
   no interactive display backend. Resume only from the user's exact failing
   GUI environment. After WS017, continue the remaining WS007 integration,
   including real Xzed/LFB behavior.
12. Continue WS001 POSIX work. Add a bounded `lp`/`lpr` Phase whose deliberate
   model posts PDF directly to an LPD printer and has no local spool queue.
13. Re-plan WS019 so the installer is written in Noct, then implement it after
   its complete installer specification is supplied. The user's latest message
   ended after "仕様は"; that missing contract is a human blocker and must not be
   guessed from the older C-oriented plan.
14. After the installer passes automatic acceptance, complete WS003 by
    installing to and booting from the Latitude 5320 NVMe device.
15. Resume WS011 VLAN and bridge work. The previous manual hold is released by
    this priority instruction, but any still-open virtual-interface, packet-
    ownership, filtering, or persistence decision remains a design gate rather
    than permission to improvise an incompatible UAPI.
16. Keep WS013 Runtime CPAR, WS014 GPU, and WS015 μITRON pending until their
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
| USB Ethernet asynchronous TX statistics | WS004 p017 | Resolved and implemented by q054 for NCM: packets/bytes count driver acceptance; later `STALL`, `TIMEOUT`, `DISCONNECTED`, or `IO_ERROR` adds exactly one `tx_errors` and no `tx_dropped`; administrative `CANCELLED` adds neither. ECM is a separate future consumer of the common helper |
| First USB WLAN identity | WS004/WS005 | Resolved in q055 and refined for q056: the Japan-labelled unit has no printed hardware revision, so its retained exact `2357:012e`, `bcdDevice=2.10`, `ff/ff/ff`, five-endpoint descriptor is authoritative. Firmware is the separately selected `userland/firmware/rtl8822b/` entry, fetched from one immutable GitHub revision and hash-verified; V1.0 remains documentary family evidence only |
| Built-in PCI WLAN identity | WS004/WS005 | Resolved as RTL8822CE `10ec:c822`, subsystem `10ec:c130`; retained as a later target after the Archer-first sequence |
| Intel test-machine identity and firmware | WS004 p037 | Resolved in q061: the AX201 hypothesis is corrected to exact AX211/CNVio2 PCI `8086:51f0`, subsystem `8086:4090`, revision `01`; selected `iwlwifi-so-a0-gf-a0-89.ucode`, PNVM, official `linux-firmware` `20260410` bytes, clear license, and direct-boot boundary are frozen in HW-T37 |
| WLAN firmware source layout | WS004/WS005 | Resolved: menuconfig divides userland into Base, X11, Firmware, and Packages. Per-device firmware entries live under `userland/firmware/rtl8822b`, future `rtl8822c`, and p038 `intelax211`; they fetch only when selected, install bytes below `/lib/firmware`, and retain the applicable license. P037 freezes AX211's exact `-89.ucode`/PNVM bytes and terms before implementation |
| Intel/Realtek WLAN commonization boundary | WS004 p039 | Resolved as late abstraction: p038 first implements AX211 independently behind the stable public WLAN UAPI. Only after both exact devices work may p039 extract substantial, stable, demonstrated common behavior; a no-extraction review is valid, and a significant public-UAPI change requires a separate explicit decision |
| WLAN privilege and process topology | WS005 | Resolved for v1: one `root:network` mode-0660 `/run/networkd.sock`, kernel-attested connection-time peer credentials, `net` as user/desktop frontend, root `networkd` as orchestrator, and fixed primitive `ifconfig`/`wifi`/`dhcpc` children; no resident/pluggable `wpa` child |
| WLAN protocol-state ownership | WS004/WS005 | Resolved for v1: a device-independent kernel WLAN layer retains scan/authentication/association/WPA2 key and rekey/controlled-port state after one-shot `/sbin/wifi` exits; the RTL8822BU driver owns only hardware/firmware-specific radio, USB, frame, channel, and key-slot operations |
| `/etc/net.conf` v1 grammar and empty-collection syntax | WS011 | Parser and boot migration |
| VLAN/bridge virtual-interface UAPI and packet ownership | WS005/WS011 | Manual hold released 2026-08-30; `ws011-p004` must now close the remaining design gates before implementation |
| Linux/FreeBSD evdev compatibility profile | WS006 | Resolved by `ws006-p001`; implement `/dev/input/eventN` against it |
| USB HID v1 policy boundary | WS006 p008 / WS004 | Resolved: use checked Report Protocol with no malformed-descriptor fallback, and reserve a detached `eventN` until the final old-generation fd closes. USB 1.1 is required, so UHCI/EHCI concurrent-request scheduling and hotplug precede p008; a general endpoint-STALL/device-reset recovery contract is also required |
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
| Authoritative Noct repository, build sequence, and release | WS008 | Resolved by q063: official `awemorris/NoctLang` release `v2.0.1`, tag commit `ed621e79139f55d06dd1a474243afbf0ce5efe0a`, archive size `2524680`, and SHA-256 `68588c84f508856474526be1c576cf6190ee99539cd81cc8453857d894f98f9f` are the common host/target identity. Both `--path` forms, toolchain/ordinary build, amd64 target package, and q35/xHCI non-JIT/JIT/BeUI gates pass. The target-only two-hunk final-link patch is explicitly not BeUI; Remacs and i386/PC-98 target support remain outside the accepted scope. |
| x86 compiler triples, bootstrap and sysroot ownership | WS021 | Resolved by q064: host C/C++ builds host Noct and verified patched LLVM 23.1.0; project LLVM installs at `build/llvm` from source or the pinned `rev-0` cache; target triples are `x86_64-unknown-zedbsd` and `i386-unknown-zedbsd`; sysroots are `build/amd64/sysroot` and shared `build/i386/sysroot`; target Noct uses the former; BIOS/UEFI loaders use LLVM with no host target GNU/MinGW fallback. The final amd64/i386/PC-98 and target-noct runtime campaign passes. sparcv9/m68030 use a later project-built GCC. |
| x86 HAL style-only boundary | WS023 | Resolved and completed in q067: `plan/coding-style.md` now applies to all 88 C/header files under `src/hal/i386` and `src/hal/amd64`; the five inherited edits were preserved, narrow compiler-extension/table exceptions are recorded, and strict, focused, configured-build, runtime, and API/ABI review passed. |
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

Released holds remain permanent history rather than reusable identifiers:

| Hold ID | Released | Result |
| --- | --- | --- |
| `MB-001` | 2026-08-30 | The user placed VLAN and bridge implementation back in the active priority order. The manual hold is removed; unresolved virtual-interface UAPI, packet ownership, filtering, and persistence details remain ordinary p004 design gates. |
| `MB-008` | 2026-08-31 | The maintainer-published `e56274ff...` revision first restored runtime `--path`; q047 retained the compile/application parser as p010's ordinary resume condition. Q063 later resolves that condition with official `v2.0.1`, completes p010/p009, and leaves no Noct human-decision hold. |
| `MB-009` | 2026-09-02 | Q061 found AX211/CNVio2 rather than the AX201 hypothesis; the user immediately accepted exact `8086:51f0`, subsystem `8086:4090`, revision `01` as the corrected target. P037 is complete and p038 is retargeted, so no Intel-target manual block remains. |
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

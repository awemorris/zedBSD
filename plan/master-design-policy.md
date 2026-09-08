# zedBSD design policy and decision reference

Last updated: 2026-09-09

masterから分離した継続的な設計方針・決定の参照資料です。個別の完了時点や旧milestoneの説明は決定当時の文脈です。
現在のWS状態・Priority・Future Listは [master](master.md) を正とします。
WS013/WS015の長期目標は将来項目であり、現行インストール作業のRuntime依存ではありません。
旧VLAN/bridgeモデルは現行WS011の完了条件ではありません。

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

## 7. Decisions that gate new Phases

| Decision | Owning WS | Required before |
| --- | --- | --- |
| I/O/cache redesign and amd64 1 GiB removal | WS025 | Resolved on 2026-09-07: buffer-plan-codex-2 direction approved; remove the diagnostic RAM limit and loader reporting truncation, use typed firmware ranges with separate permanent RAM mapping and constrained DMA; detailed M/W/P recorded; autonomous execution authorized, p001--p010 completed under q088--q097; q098 p012 complete; q099 p014 complete |
| Exact Latitude BIOS, boot mode, PCI/USB topology and IDs | WS003 | Driver selection and hardware acceptance |
| Exact CF-SV7 DMI identity, firmware settings, CPU/APIC mode, PCI/USB topology, and IDs | WS003 | Later device-specific driver selection; p020 early IRQ and p021 portable-GPT work do not depend on the remaining inventory |
| Intel Mac identity, firmware, and target-medium inventory | WS020 | p004 physical acceptance only; no exact capacity match is required because the fixed GPT extent may precede the physical end |
| UEFI-only Protective MBR and primary-only GPT shape | WS020 | Resolved: zero non-executable Protective-MBR code, one non-active `0xee` entry, three zero entries, and `55 aa`; the fixed 395,297-sector artifact contains a valid primary GPT and zero final 33-sector reservation but no backup GPT or compatibility-MBR entry |
| Initial Secure Boot scope | WS003 | Resolved: use UEFI with Secure Boot disabled; signing/key enrollment deferred and not required for NVMe |
| USB Ethernet interface descriptors and, for vendor-specific interfaces, VID:PID/controller family | WS003/WS004/WS005 | Choose CDC ECM/NCM class frontend or Realtek-family frontend for HW-12/NET-10; ACM is inapplicable |
| USB Ethernet asynchronous TX statistics | WS004 p017 | Resolved and implemented by q054 for NCM: packets/bytes count driver acceptance; later `STALL`, `TIMEOUT`, `DISCONNECTED`, or `IO_ERROR` adds exactly one `tx_errors` and no `tx_dropped`; administrative `CANCELLED` adds neither. ECM is a separate future consumer of the common helper |
| First USB WLAN identity | WS004/WS005 | Resolved in q055 and refined for q056: the Japan-labelled unit has no printed hardware revision, so its retained exact `2357:012e`, `bcdDevice=2.10`, `ff/ff/ff`, five-endpoint descriptor is authoritative. Firmware is the separately selected `userland/firmware/rtl8822b/` entry, fetched from one immutable GitHub revision and hash-verified; V1.0 remains documentary family evidence only |
| Additional Archer T3U Plus identity | WS004 p045/p046 | Feasibility verified for exact `2357:0138`: HS `0210/0210`, 512-byte bulk endpoints; after Linux mode switch SS `0300/0300`, 1024-byte bulk and measured companions. Existing pinned firmware initializes and passively scans on Linux. Q081 cell 4 proves both-band zedBSD data on the measured cut-D/RFE3 board at SuperSpeed; q083/p046 is complete after three cycles per band and final fresh reopen/down; actual 5-GHz data acceptance is 20-MHz W52 channel 44 |
| Built-in PCI WLAN identity | WS004/WS005 | Resolved as RTL8822CE `10ec:c822`, subsystem `10ec:c130`; retained as a later target after the Archer-first sequence |
| Intel test-machine identity and firmware | WS004 p037 | Resolved in q061: the AX201 hypothesis is corrected to exact AX211/CNVio2 PCI `8086:51f0`, subsystem `8086:4090`, revision `01`; selected `iwlwifi-so-a0-gf-a0-89.ucode`, PNVM, official `linux-firmware` `20260410` bytes, clear license, and direct-boot boundary are frozen in HW-T37 |
| WLAN firmware source layout | WS004/WS005 | Resolved: menuconfig divides userland into Base, X11, Firmware, and Packages. Per-device firmware entries live under `userland/firmware/rtl8822b`, future `rtl8822c`, and p038 `intelax211`; they fetch only when selected, install bytes below `/lib/firmware`, and retain the applicable license. P037 freezes AX211's exact `-89.ucode`/PNVM bytes and terms before implementation |
| Intel/Realtek WLAN commonization boundary | WS004 p039 | Resolved as late abstraction: p038 first implements AX211 independently behind the stable public WLAN UAPI. Only after both exact devices work may p039 extract substantial, stable, demonstrated common behavior; a no-extraction review is valid, and a significant public-UAPI change requires a separate explicit decision |
| WLAN privilege and process topology | WS005 | Resolved for v1: one `root:network` mode-0660 `/run/networkd.sock`, kernel-attested connection-time peer credentials, `net` as user/desktop frontend, root `networkd` as orchestrator, and fixed primitive `ifconfig`/`wifi`/`dhcpc` children; no resident/pluggable `wpa` child |
| WLAN protocol-state ownership | WS004 p044 / WS005 p010 | Resolved for v1: the device-independent kernel WLAN layer owns asynchronous scan, one-attempt authentication/association, WPA2 key/rekey, controlled-port, terminal link-loss, and link-event state; `/sbin/wifi` alone owns one monotonic 30-second high-level scan/select/connect retry sequence; the driver owns only hardware/firmware-specific operations |
| Public WLAN grammar and selection | WS005 p002/p007 | Resolved on 2026-09-05: exactly `set-key SSID PASSPHRASE [auto]`, `enable`, `disable`, `list`, `connect SSID`, and `disconnect`, with no public interface operand. Omitted `auto` means manual. There is at most one global connection. Automatic selection uses profile-file order then the first stable-discovery-order WLAN reporting that candidate; manual selection uses the exact saved SSID and the same interface rule |
| WLAN policy owner and secret lifetime | WS005 p005-p007/p011 | Resolved on 2026-09-05: `net` is stateless; `enable` first validates the authenticated peer euid's fixed store and a stable radio enumeration without mutation, publishes that euid as active owner only after policy preparation succeeds, and permits root override. Networkd never accepts a UID/path/interface/passphrase/profile in ZNV2 and wipes the operation-local passphrase after each fd-4 child. The file is the sole long-lived secret owner |
| Managed WLAN reconnect and lifecycle state | WS005 p011 | Resolved: networkd owns `disabled`, `auto-searching`, `connected`, and `manual-disconnected`, with bounded internal `connecting`/`reconnecting` transients. Zero radios is a successful `auto-searching` hotplug wait; partial multi-radio preparation continues with usable radios in stable order. RF loss enters `reconnecting`, rereads the current store, and runs exactly one ordinary 30-second `/sbin/wifi` child for the same selected SSID; success returns to `connected`, while failure cleans up and settles in `auto-searching`. Explicit disconnect leaves radios up/scanning while suppressing automatic selection, and disable removes the policy. Startup and normal exit must disconnect, stop scans, and lower every detected WLAN; failed normalization cannot publish `READY` or a successful exit |
| Kernel interface link notification transport | WS004 p044 / WS005 p011 | Resolved for q071: use read-only `socket(PF_ROUTE, SOCK_RAW, 0)` with bounded versioned `RTM_IFINFO` carrier/removal records, poll wakeup, explicit overflow/resnapshot, ifindex plus device-generation identity, and no route-mutation API in this Phase |
| `/etc/net.conf` v1 grammar and empty-collection syntax | WS011 | Parser and boot migration |
| VLAN/bridge virtual-interface UAPI and packet ownership | WS005/WS011 | 2026-09-09: VLAN cancelled; bridge transferred to Future List F-001. The combined p004 must not be resumed. |
| Linux/FreeBSD evdev compatibility profile | WS006 | Resolved by `ws006-p001`; implement `/dev/input/eventN` against it |
| USB HID v1 policy boundary | WS006 p008 / WS004 | Resolved: use checked Report Protocol with no malformed-descriptor fallback, and reserve a detached `eventN` until the final old-generation fd closes. USB 1.1 is required, so UHCI/EHCI concurrent-request scheduling and hotplug precede p008; a general endpoint-STALL/device-reset recovery contract is also required |
| Device-mapping `mprotect` ceiling | WS017 | Resolved on 2026-09-05: permissions may return only within the original mapping maximum (`RW -> RO -> RW` is valid for an initially RW map); an initially RO mapping cannot gain write and no device mapping can gain execute |
| zedBSD GPU/Vulkan capability, object, and display-takeover profile | WS014 | Manually blocked; publishing `/dev/gpuN` UAPI or transferring i915 ownership |
| YAML `/etc/rc.conf` schema and versioned init status/control protocol | WS012 | Resolved and complete: q017 completed YAML/persistence; q018 completed typed `/run/init.sock` service and `ZSV1 HALT`/`POWEROFF`/`REBOOT` clients, argv/interactive administration, and production integration with no unversioned compatibility path |
| x86 kernel boot-parameter contract | WS003/WS013 | Resolved and implemented by q015: `boot0`--`boot3`, exclusive `rootpart` or explicit overlay root/data, `swap0`--`swap3`, and `init`; BR-T46 passes all 31 four-platform QEMU cells |
| UEFI `zedbsd.cfg` boot contract | WS013 | Resolved for q031: required same-disk FAT16/FAT32 `/zedbsd.cfg`, required loader-only `kernel=`, direct kernel parameters with bounded shorthand, overlay or native `rootpart`, no menu, and ignored UEFI LoadOptions |
| BIOS boot configuration names and convergence | WS013 p005/p006 | Resolved and implemented by q032: i386 PC/AT PBR/`BOOTZBSD.EXE` and amd64 BIOS PBR/`BOOTZBSD.EXE` use `/zedbsd.cfg`; PC-98 `BOOTZBSD.EXE` uses `/BOOTZBSD.CFG`; all implement the p003 format and parameter result with no `boot.cfg`, fixed-kernel, embedded-record, or reserved-area direct-kernel fallback |
| NVMe partition naming | WS004/WS019 | Resolved by the existing one-based disk contract: namespace is `/dev/nvme0n1`, first partition is `p1`; the earlier `p0` example is not a new ABI |
| Installer v1 layout and mutation boundary | WS019 p001 | Resolved: existing GPT, exactly one existing ESP, one explicitly selected distinct same-disk FAT32, no partition-format/GPT/label writes, overlay files only, and no firmware-variable mutation; target commands format only newly created contained files |
| Installer read-only administration UAPI | WS019 p002 | Expose stable whole/partition identity, GPT type/PARTUUID, parent relation, capacity, filesystem type, mount/swap state, and loader-origin identity before read-only `diskpart` and `zedinst` preflight |
| Installer payload discovery | WS013 p002 | Resolved for q031: search same-physical-disk FAT16/FAT32; zero `/zedbsd.cfg` candidates is fatal, multiple candidates warn and use the deterministic first, and omitted `boot0` defaults to the selected config FAT while an explicit value is preserved |
| Installed UEFI `LoadOptions` precedence | WS013 p002/p003 | Resolved for q031: ignore LoadOptions on the required `zedbsd.cfg` path; do not merge or override the configuration |
| Installer source-image stability | WS019 p004/p008/p009 | Resolved on 2026-09-05: no templates. `zedinst` creates unpublished regular files and invokes target `/sbin/mkfs` for existing UFS1 and `/sbin/mkswap` for existing ZEDSWAP2; it never copies the live overlay upper or active swap |
| UFS1/UFS2 consolidation | WS024 / WS018 / WS019 | Resolved on 2026-09-06: one filesystem named UFS with one 64-bit implementation on both 32-bit and 64-bit CPUs. Use the current UFS2 codec as the implementation starting point, preserve required features, and migrate driver/formatter/boot/image consumers together. P001 freezes disk identification, limits and old-image handling; separate permanent UFS1/UFS2 implementations are no longer the target. Contract frozen by q100; implementation active in q101 as a WS025 dependency. |
| Runtime CPAR namespace/security, CLI/build, and service-package contracts | WS013 | 2026-09-09: Future List F-002; reconsider the Runtime contracts only when selected again. |
| Confirmed-commit ownership and bounds | WS011 p005--p008 | Resolved on 2026-09-05: interactive originating `net` owns candidate/token in memory and alone writes `/etc/net.conf` on ordinary commit; networkd owns only the volatile timer/open `/tmp` rollback program and never touches that file; client loss makes confirmation/adoption impossible, while explicit rollback or timeout remains; p005 freezes all size/time/lock/acknowledgement bounds; p007 owns the completed automatic QEMU evidence; p008 is user-accepted as complete on 2026-09-09 |
| Authoritative Noct repository, build sequence, and release | WS008 | Resolved by q063: official `awemorris/NoctLang` release `v2.0.1`, tag commit `ed621e79139f55d06dd1a474243afbf0ce5efe0a`, archive size `2524680`, and SHA-256 `68588c84f508856474526be1c576cf6190ee99539cd81cc8453857d894f98f9f` are the common host/target identity. Both `--path` forms, toolchain/ordinary build, amd64 target package, and q35/xHCI non-JIT/JIT/BeUI gates pass. The target-only two-hunk final-link patch is explicitly not BeUI; Remacs and i386/PC-98 target support remain outside the accepted scope. |
| x86 compiler triples, bootstrap and sysroot ownership | WS021 | Resolved by q064: host C/C++ builds host Noct and verified patched LLVM 23.1.0; project LLVM installs at `build/llvm` from source or the pinned `rev-0` cache; target triples are `x86_64-unknown-zedbsd` and `i386-unknown-zedbsd`; sysroots are `build/amd64/sysroot` and shared `build/i386/sysroot`; target Noct uses the former; BIOS/UEFI loaders use LLVM with no host target GNU/MinGW fallback. The final amd64/i386/PC-98 and target-noct runtime campaign passes. sparcv9/m68030 use a later project-built GCC. |
| x86 HAL style-only boundary | WS023 | Resolved and completed in q067: `plan/coding-style.md` now applies to all 88 C/header files under `src/hal/i386` and `src/hal/amd64`; the five inherited edits were preserved, narrow compiler-extension/table exceptions are recorded, and strict, focused, configured-build, runtime, and API/ABI review passed. |
| PC/AT boot selector | WS004 | Resolved: reuse UUID/PARTUUID; standard FAT handoff uses UUID |
| Runtime swap command standard and control boundary | WS016 | Resolved for v1: SUSv4/POSIX does not define `swapon`/`swapoff`; zedBSD supplies minimal privileged extensions over versioned `/dev/system` control and existing signed sources |
| Optional LFB mapping and Xzed fallback boundary | WS017 | Resolved for v1: fixed post-ENTER geometry, 8/16/24/32-bpp layout query, shared non-executable mmap when supported, true-color Xzed fast path, and unchanged ioctl fallback; PC-98 Cirrus is excluded |
| μITRON compatibility and RT isolation contract | WS015 | Future Listへ移動（2026-09-09）。再開時の設計事項として保持: user-mode resident ELF and explicit MMIO grants are fixed, but exact profile, legacy static configuration, RT CPU/IRQ/timer model, RT/POSIX mailbox/filesystem ownership, limited POSIX-failure recovery, and first board-specific latency target remain required before any implementation Phase |

### 7.1 Manual blocking register

以下は設計保留の識別子を保持する参照表です。WS013/WS015はFuture Listへ移動済み、
MB-010はVLANキャンセル・bridge将来移管により旧一括保留として終了しました。
現在の着手対象と再開判断は [master](master.md) を優先します。

| Hold ID | Owning WS/Phase | Manually blocked topic | Resume condition |
| --- | --- | --- | --- |
| `MB-002` | `ws013-p001` | Runtime CPAR namespace, isolation, and security model | Future Listから対象を明示選択した時点で再設計 |
| `MB-003` | `ws013-p001` | `cpar run`, `cpar sh`, and `cpar build` grammar/lifecycle | Future Listから対象を明示選択した時点で再設計 |
| `MB-004` | `ws013-p001` | Service-container package format, dependencies, updates, config, and data | Future Listから対象を明示選択した時点で再設計 |
| `MB-005` | `ws014-p001` | GPU UAPI, capability profiles, display takeover, i915 split, Vulkan/GLES | User explicitly resumes GPU architecture discussion |
| `MB-007` | `ws015-p001` | μITRON profile/UAPI, legacy static configuration, RT/POSIX mailbox and filesystem proxy, scheduling, failure, and timing contracts | Future Listから対象を明示選択した時点で再設計 |
| `MB-010` | `ws011-p004` | 旧VLAN/bridge一括保留は終了 | VLANはキャンセル。bridgeのみmasterのFuture List F-001へ移管。旧Phase全体を再開しない |

Released holds remain permanent history rather than reusable identifiers:

| Hold ID | Released | Result |
| --- | --- | --- |
| `MB-001` | 2026-08-30 | The user placed VLAN and bridge implementation back in the active priority order. The manual hold is removed; unresolved virtual-interface UAPI, packet ownership, filtering, and persistence details remain ordinary p004 design gates. |
| `MB-008` | 2026-08-31 | The maintainer-published `e56274ff...` revision first restored runtime `--path`; q047 retained the compile/application parser as p010's ordinary resume condition. Q063 later resolves that condition with official `v2.0.1`, completes p010/p009, and leaves no Noct human-decision hold. |
| `MB-009` | 2026-09-02 | Q061 found AX211/CNVio2 rather than the AX201 hypothesis; the user immediately accepted exact `8086:51f0`, subsystem `8086:4090`, revision `01` as the corrected target. P037 is complete and p038 is retargeted, so no Intel-target manual block remains. |
| `MB-006` | 2026-08-30 | The user resumed WLAN design after completing USB Ethernet. The old RTL8822CE-first, `/sbin/wpa`, and `/etc/wpa/` proposal was superseded by the Archer T3U Nano first target and the fixed `net` -> `networkd` -> `ifconfig`/`wifi`/`dhcpc` topology. Firmware and exact-device facts are ordinary Phase dependencies, not a continuing manual hold. |


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


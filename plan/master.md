# zedBSD master plan

Last updated: 2026-08-27

Status: active

## 1. Purpose

This is the authoritative program-wide plan. It assigns permanent workstream
IDs, records the current stop/resume point of every WS, and defines dependencies
between WSs. Detailed scope belongs in each `ws.md`; implementation design,
acceptance, results, and interruption state belong in each `phase.md`.

The immediate north star is:

> Boot zedBSD from USB on a Dell Latitude 5320, reach a usable local shell, and
> establish a working network path with reproducible evidence.

The current [Queue Book](queue.md) is `q017`, finished, and retained as
[q017](queue-q017.md). It completed `ws012-p002`: the strict YAML rc.conf host
fixtures, production build, and disposable amd64 QEMU persistence/reboot proof
all passed. The preceding [q016](queue-q016.md) completed `ws004-p009`.
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

### 2.3 Program completion direction

The project advances toward the final goal when the base system can be built
from its own maintained source, boots reliably on supported targets, provides
the documented UNIX/POSIX-oriented interfaces, and supports product-specific
package sets for all four target classes. Individual WSs may complete or pause
before this long-term product goal is reached.

## 3. Workstream registry

| WSID | Workstream | Status | Last completed / current Phase | Resume point | WS plan |
| --- | --- | --- | --- | --- | --- |
| `ws001` | POSIX.1-2024 compliance | Paused, ledger active | `ws001-p013` complete | Select `cksum` or another bounded tier-1 candidate | [WS001](ws001-posix/ws.md) |
| `ws002` | System services | Complete baseline | `ws002-p020` complete with handoffs | New networking work resumes in WS005 | [WS002](ws002-services/ws.md) |
| `ws003` | Dell Latitude 5320 bring-up | Active; physical U3/q014 and q015 p011--p015 complete; p016 Queue-ready | `ws003-p015` completed with BR-T46 31/31 | Run `ws003-p016` to isolate generated boot-parameter inputs, then extract physical U4/U5 work; BR-T30 repeatability, BR-T31 sustained I/O, and hardware inventory remain | [WS003](ws003-bringup/ws.md) |
| `ws004` | Hardware expansion | Active; checked legacy-HCD IRQ residual complete in `q016` | `ws004-p009` complete; prior automatic USB milestones remain complete | Select the next dependency-ready hardware Phase or record later manual USB evidence; MSI-less xHCI policy remains independent | [WS004](ws004-hardware/ws.md) |
| `ws005` | Networking and WPA | Planned; USB Ethernet first, WLAN manually blocked | WS002 Phase 20 is the inherited baseline | Classify one USB Ethernet descriptor, then extract the wired physical-network Phase | [WS005](ws005-networking/ws.md) |
| `ws006` | Input and evdev | In progress | `ws006-p004` complete PC/AT software milestone | Select Xzed migration after xHCI/USB-HID dependencies, retaining PC-98/X68000 physical-token follow-up | [WS006](ws006-input/ws.md) |
| `ws007` | Graphics and desktop | Paused | `ws007-p001` complete; `p002` carried | Resume mouse work with evdev/absolute input or a concrete reproducer | [WS007](ws007-graphics/ws.md) |
| `ws008` | Noct and BeUI | Planned; three implementation Phases Queue-ready | No Phase started; `ws008-p001` is dependency-ready | Queue p001 for canonical `cmake --preset zedbsd`, then p002 BeUI and p003 amd64 JIT in dependency order | [WS008](ws008-noct/ws.md) |
| `ws009` | Documentation | In progress | `ws009-p003` complete | Extract the next dependency-ready producer-linked reference | [WS009](ws009-documentation/ws.md) |
| `ws010` | Noct scripting and x86 image tools | Complete | `ws010-p001`–`p004` complete | Noct toolchain and the 15-script x86 production closure are complete; three images boot to `login:` | [WS010](ws010-scripting/ws.md) |
| `ws011` | Network configuration console | In progress; confirmed-commit public semantics fixed | `ws011-p003` complete; p005 bounds open; p004 manually blocked | Freeze p005 timeout/lock/diagnostic bounds; do not resume VLAN/bridge without explicit release | [WS011](ws011-net-config/ws.md) |
| `ws012` | Service administration console | Active; p002 complete in `q017`, p003-p006 Queue-ready in order | `ws012-p002` complete | Execute ZSV1 service/system actions, argv CLI, console, and integration Phases beginning with p003 | [WS012](ws012-service-console/ws.md) |
| `ws013` | CPAR container partitioning | Proposed; Boot v1 grammar fixed, Runtime topics manually blocked | `ws013-p001` is the only current Phase | Resolve bounded UEFI FAT LFN/parser/menu details until Runtime CPAR holds are released | [WS013](ws013-containers/ws.md) |
| `ws014` | Native GPU stack | Blocked by manual hold | `ws014-p001` is blocked before detailed design | Resume only after explicit user release | [WS014](ws014-gpu/ws.md) |
| `ws015` | μITRON asymmetric real-time domain | Blocked by manual hold `MB-007`; user-mode RT direction recorded | `ws015-p001` is the only current Phase | After explicit hold release, select the μITRON profile and freeze the remaining RT/POSIX, mailbox/filesystem, failure, and timing contracts | [WS015](ws015-muitron-rt/ws.md) |
| `ws016` | Runtime swap control | Planned; four implementation Phases Queue-ready | No Phase started; `ws016-p001` is dependency-ready | Queue p001 runtime manager, followed by UAPI, commands, and amd64 QEMU acceptance | [WS016](ws016-swap-control/ws.md) |
| `ws017` | `/dev/graphics` LFB fast path | Planned; four implementation Phases Queue-ready | No Phase started; `ws017-p001` is dependency-ready | Queue p001 device-mmap/UAPI, then amd64 backend, Xzed fast path, and UEFI acceptance | [WS017](ws017-lfb-graphics/ws.md) |

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
| Continuous | POSIX debt and public documentation remain traceable | WS001, WS009, all producers |

## 5. Dependency map

```text
WS002 service baseline
  +-- WS005 networkd/net/WPA expansion
  +-- WS011 net console + /etc/net.conf
  +-- WS012 service administration console -- WS013 service containers

WS011 configuration model
  +-- WS005 physical network/WPA backends
  +-- WS011 VLAN/bridge data path (joint UAPI review with WS005)

WS003 hardware inventory
  +-- WS004 xHCI + USB storage -- WS003 QEMU/Latitude USB root
  |                                  +-- USB Ethernet -- WS003/WS005 network
  +-- WS004 PCIe/DMA/interrupts
       +-- NVMe
       +-- RTL8822CE WLAN -- WS005 wpa/networkd (manual hold MB-006)
       +-- i915 prerequisites -- WS014 GPU/Vulkan/GLES -- WS007 Wayland

WS004 xHCI -- WS006 USB HID -- evdev
                                  +-- WS007 X11/Wayland
                                  +-- WS008 BeUI

WS003 p014/p015 signed swap sources + VM/VFS ownership
  +-- WS016 runtime swap manager -- /dev/system UAPI -- swapon/swapoff

WS007 Xzed + /dev/graphics + VM device mappings
  +-- WS017 optional LFB mmap -- Xzed fast path -- amd64 UEFI acceptance

WS013 Boot CPAR -- WS003 p011-p015 common x86 parameters
                  +-- WS003/bootloader FAT32, LFN and boot.cfg menu
WS013 Runtime CPAR -- WS012 service administration
                   +-- WS005 optional network profiles
                   +-- WS004 persistent image storage

WS004 SMP/IRQ/timer/reset + WS001 POSIX boundary
  +-- WS015 μITRON asymmetric RT domain -- WS009 public API and timing contract

WS001 compliance and WS009 documentation cross all workstreams.
WS010 supplies host-side build and test scripting used by all workstreams.
```

## 6. Priority waves

1. Preserve M0 and capture the exact Latitude hardware inventory.
2. Implement QEMU xHCI USB-root boot and stable boot-device selection.
3. Reach a Latitude USB-root login shell and establish diagnostics.
4. Bring up USB Ethernet as the first physical network path, plus NVMe, evdev,
   and USB HID.
5. Add the optional LFB Xzed path and the upstream Noct target/BeUI/JIT
   sequence when selected. Independently, after `MB-006` is released, add the
   RTL8822CE driver and pluggable WPA path.
6. After its manual hold is released, complete WS014 GPU architecture
   discussion, then freeze the GPU UAPI and implement i915, Vulkan, and OpenGL
   ES 2.0 through separately authorized Phases.
7. Implement the Wayland compositor/desktop environment.
8. Complete the unblocked UEFI Boot CPAR design. Runtime CPAR and service
   containers remain outside implementation priority until their manual holds
   are released.
9. After `MB-007` is released, complete WS015's μITRON/RT architecture
   discussion; after its profile, target, isolation, mailbox/filesystem,
   failure, and evidence decisions are fixed, schedule bounded RT
   implementation Phases without displacing earlier physical-network and
   storage milestones.

The current dependency-ready planning pool also contains `ws003-p016`,
`ws008-p001`, `ws012-p003`, `ws016-p001`, and `ws017-p001`. They may be
selected into a later finite Queue according to the user's next priority; this
list does not itself authorize implementation.

WS001 and WS009 advance within every wave when bounded work is selected. Lower
priority POSIX gaps may remain paused if they do not block the active milestone.

## 7. Decisions that gate new Phases

| Decision | Owning WS | Required before |
| --- | --- | --- |
| Exact Latitude BIOS, boot mode, PCI/USB topology and IDs | WS003 | Driver selection and hardware acceptance |
| Initial Secure Boot scope | WS003 | Resolved: use UEFI with Secure Boot disabled; signing/key enrollment deferred and not required for NVMe |
| USB Ethernet interface descriptors and, for vendor-specific interfaces, VID:PID/controller family | WS003/WS004/WS005 | Choose CDC ECM/NCM class frontend or Realtek-family frontend for HW-12/NET-10; ACM is inapplicable |
| Built-in PCI WLAN identity | WS004/WS005 | Resolved as RTL8822CE `10ec:c822`, subsystem `10ec:c130`; implementation and firmware packaging are manually blocked by `MB-006` |
| `/etc/net.conf` v1 grammar and empty-collection syntax | WS011 | Parser and boot migration |
| VLAN/bridge virtual-interface UAPI and packet ownership | WS005/WS011 | Manually blocked; `ws011-p004` discussion and implementation |
| Linux/FreeBSD evdev compatibility profile | WS006 | Resolved by `ws006-p001`; implement `/dev/input/eventN` against it |
| zedBSD GPU/Vulkan capability, object, and display-takeover profile | WS014 | Manually blocked; publishing `/dev/gpuN` UAPI or transferring i915 ownership |
| YAML `/etc/rc.conf` schema and versioned init status/control protocol | WS012 | Resolved: q017 completed YAML/persistence, and p003 uses the same `/run/init.sock` for `ZSV1 HALT`/`POWEROFF`/`REBOOT` while migrating all service and shutdown clients without an unversioned compatibility path |
| x86 kernel boot-parameter contract | WS003/WS013 | Resolved and implemented by q015: `boot0`--`boot3`, exclusive `rootpart` or explicit overlay root/data, `swap0`--`swap3`, and `init`; BR-T46 passes all 31 four-platform QEMU cells |
| UEFI Boot CPAR `boot.cfg`/LFN/menu contract | WS013 | Section grammar maps to the common parameter contract; freeze bounded FAT/parser/menu details before implementation; legacy PC/AT and PC-98 menus are excluded |
| Runtime CPAR namespace/security, CLI/build, and service-package contracts | WS013 | Manually blocked; any Runtime CPAR implementation Phase |
| Confirmed-commit implementation bounds | WS011 | Public semantics are fixed: interactive only, explicit timeout, delayed `/etc/net.conf` write, ordinary `commit` confirms, and DHCP is reacquired; freeze timeout maximum, lock path, and diagnostic bounds before implementation |
| Authoritative Noct repository, build sequence, and revision | WS008 | Resolved: official and integration trees are pinned at `7d856856e16eb2d889ba49f557f2fda4dcaeea7e`; Queue-ready order is zedBSD CMake preset, canonical BeUI backend, then amd64 JIT acceptance |
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
| `MB-001` | `ws011-p004` | VLAN/bridge detailed design and implementation | User explicitly resumes VLAN/bridge discussion |
| `MB-002` | `ws013-p001` | Runtime CPAR namespace, isolation, and security model | User explicitly resumes Runtime CPAR namespace discussion |
| `MB-003` | `ws013-p001` | `cpar run`, `cpar sh`, and `cpar build` grammar/lifecycle | User explicitly resumes Runtime CPAR CLI/build discussion |
| `MB-004` | `ws013-p001` | Service-container package format, dependencies, updates, config, and data | User explicitly resumes service-container package discussion |
| `MB-005` | `ws014-p001` | GPU UAPI, capability profiles, display takeover, i915 split, Vulkan/GLES | User explicitly resumes GPU architecture discussion |
| `MB-006` | `ws004`/`ws005` | RTL8822CE driver, firmware acquisition/republication policy, WPA database/backend, and WLAN integration | User explicitly resumes WLAN discussion after the USB Ethernet milestone |
| `MB-007` | `ws015-p001` | μITRON profile/UAPI, legacy static configuration, RT/POSIX mailbox and filesystem proxy, scheduling, failure, and timing contracts | User explicitly resumes WS015 architecture/API discussion |

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
- a requested compatibility target conflicts with an explicit zedBSD design
  policy.
- a hard-real-time or POSIX-crash-survival claim cannot be supported by the
  selected core, interrupt, firmware, memory, or shared-kernel isolation model.

# zedBSD master plan

Last updated: 2026-08-26

Status: active

## 1. Purpose

This is the authoritative program-wide plan. It assigns permanent workstream
IDs, records the current stop/resume point of every WS, and defines dependencies
between WSs. Detailed scope belongs in each `ws.md`; implementation design,
acceptance, results, and interruption state belong in each `phase.md`.

The immediate north star is:

> Boot zedBSD from USB on a Dell Latitude 5320, reach a usable local shell, and
> establish a working network path with reproducible evidence.

The latest execution record is [queue.md](queue.md)
(`q010`, finished; automatic USB/heap gate passed). Closed archived records are
retained as [q001](queue-q001.md), [q002](queue-q002.md),
[q003](queue-q003.md), [q004](queue-q004.md),
[q005](queue-q005.md), [q006](queue-q006.md),
[q007](queue-q007.md), [q008](queue-q008.md), and
[q009](queue-q009.md).

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
| `ws003` | Dell Latitude 5320 bring-up | Active; UEFI entry Uncleared | `ws003-p002` software correction complete, physical rerun pending | Run the corrected image on Latitude; inventory remains incomplete | [WS003](ws003-bringup/ws.md) |
| `ws004` | Hardware expansion | Active; automatic USB gate cleared | `ws004-p008` and resumed `p006` automatic milestones complete | Record detailed manual USB acceptance or extract the next hardware Phase | [WS004](ws004-hardware/ws.md) |
| `ws005` | Networking and WPA | Planned | WS002 Phase 20 is the inherited baseline | Start physical-network diagnostic Phase after inventory | [WS005](ws005-networking/ws.md) |
| `ws006` | Input and evdev | In progress | `ws006-p004` complete PC/AT software milestone | Select Xzed migration after xHCI/USB-HID dependencies, retaining PC-98/X68000 physical-token follow-up | [WS006](ws006-input/ws.md) |
| `ws007` | Graphics and desktop | Paused | `ws007-p001` complete; `p002` carried | Resume mouse work with evdev/absolute input or a concrete reproducer | [WS007](ws007-graphics/ws.md) |
| `ws008` | Noct and BeUI | Blocked before Phase | No Phase started | Obtain the authoritative Noct tree/revision | [WS008](ws008-noct/ws.md) |
| `ws009` | Documentation | In progress | `ws009-p003` complete | Extract the next dependency-ready producer-linked reference | [WS009](ws009-documentation/ws.md) |
| `ws010` | Noct scripting and x86 image tools | Complete | `ws010-p001`–`p004` complete | Noct toolchain and the 15-script x86 production closure are complete; three images boot to `login:` | [WS010](ws010-scripting/ws.md) |
| `ws011` | Network configuration console | In progress | `ws011-p003` software milestone complete | Retain migrated-DHCP QEMU evidence; wait for VLAN/bridge kernel dependencies | [WS011](ws011-net-config/ws.md) |

## 4. Milestones

| Milestone | Required result | Owning WSs |
| --- | --- | --- |
| M0 — Baseline preserved | Current QEMU boot, init, login, shell, and service behavior remains usable | WS001, WS002 |
| M1 — QEMU USB root | Automatic milestone complete: identity/reboot, URB and heap corrections, controls, and 500 pristine-copy boots pass; detailed manual acceptance pending | WS003, WS004 |
| M2 — Latitude USB shell | Uncleared at U0: UEFI loader halts during memory-map normalization before kernel entry | WS003, WS004, WS009 |
| M3 — Latitude network | At least one documented physical interface configures and transfers data | WS003, WS004, WS005 |
| M4 — Native platform devices | NVMe, USB HID, and the selected WLAN work on the target | WS004, WS005, WS006 |
| M5 — Application environments | X11 is usable and Noct/BeUI supports zedBSD upstream | WS006, WS007, WS008 |
| M6 — Accelerated graphics | `/dev/gpu0`, i915, the declared Vulkan profile, and GLES-on-Vulkan operate coherently | WS004, WS007 |
| M7 — Wayland desktop | A zedBSD Wayland compositor/DE runs on the supported input and graphics stacks | WS006, WS007 |
| Continuous | POSIX debt and public documentation remain traceable | WS001, WS009, all producers |

## 5. Dependency map

```text
WS002 service baseline
  +-- WS005 networkd/net/WPA expansion
  +-- WS011 net console + /etc/net.conf

WS011 configuration model
  +-- WS005 physical network/WPA backends
  +-- WS011 VLAN/bridge data path (joint UAPI review with WS005)

WS003 hardware inventory
  +-- WS004 xHCI + USB storage -- WS003 QEMU/Latitude USB root
  |                                  +-- WS003 diagnostics/network
  +-- WS004 PCIe/DMA/interrupts
       +-- NVMe
       +-- WLAN -- WS005 wpa/networkd
       +-- i915 -- WS007 GPU/Vulkan/GLES/Wayland

WS004 xHCI -- WS006 USB HID -- evdev
                                  +-- WS007 X11/Wayland
                                  +-- WS008 BeUI

WS001 compliance and WS009 documentation cross all workstreams.
WS010 supplies host-side build and test scripting used by all workstreams.
```

## 6. Priority waves

1. Preserve M0 and capture the exact Latitude hardware inventory.
2. Implement QEMU xHCI USB-root boot and stable boot-device selection.
3. Reach a Latitude USB-root login shell and establish diagnostics.
4. Bring up a physical network path, NVMe, evdev, and USB HID.
5. Add the selected WLAN driver and pluggable WPA path; repair X11 and add the
   upstream Noct target/BeUI backend.
6. Freeze the GPU UAPI, implement i915, Vulkan, and OpenGL ES 2.0.
7. Implement the Wayland compositor/desktop environment.

WS001 and WS009 advance within every wave when bounded work is selected. Lower
priority POSIX gaps may remain paused if they do not block the active milestone.

## 7. Decisions that gate new Phases

| Decision | Owning WS | Required before |
| --- | --- | --- |
| Exact Latitude BIOS, boot mode, PCI/USB topology and IDs | WS003 | Driver selection and hardware acceptance |
| Initial Secure Boot scope | WS003 | Freezing the USB image matrix |
| USB CDC ACM versus ECM/NCM and device-role capability | WS003/WS005 | Any CDC implementation Phase |
| WLAN controller and firmware policy | WS004/WS005 | WLAN driver/backend Phases |
| `/etc/net.conf` v1 grammar and empty-collection syntax | WS011 | Parser and boot migration |
| VLAN/bridge virtual-interface UAPI and packet ownership | WS005/WS011 | `ws011-p004` implementation |
| Linux/FreeBSD evdev compatibility profile | WS006 | Resolved by `ws006-p001`; implement `/dev/input/eventN` against it |
| zedBSD GPU/Vulkan capability profile | WS007 | Publishing `/dev/gpuN` UAPI |
| Authoritative Noct repository and revision | WS008 | First Noct implementation Phase |
| PC/AT boot selector | WS004 | Resolved: reuse UUID/PARTUUID; standard FAT handoff uses UUID |

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

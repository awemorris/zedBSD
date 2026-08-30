# WS007: graphics and desktop

Last updated: 2026-08-31

WSID: `ws007`

Status: active; X11 launch repaired, the amd64 mouse report remains carried,
and the independently reproduced PC-98 PIC-cascade regression is in q039

Parent: [master plan](../master.md)

Last verified Phase: `ws007-p001` complete; `ws007-p002` carried forward;
`ws007-p003` in progress

Resume point: complete p003's bounded PC-98 cascade repair and production
qemu-pc98 cursor regression. The different amd64 p002 report remains carried.

Shared tests: [WS007 test index](tests/README.md)

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws007-p001`](phase001-x11-launch/phase.md) | Complete | PATH script lookup repaired; production `startx` launches the four-program session |
| [`ws007-p002`](phase002-x11-mouse/phase.md) | Carried forward | Relative tracking/bounds pass; reported mismatch not reproduced and absolute input unavailable |
| [`ws007-p003`](phase003-pc98-xzed-mouse-pic-cascade/phase.md) | In progress (`q039`) | QEMU proves slave IRQ13 is pending behind masked master cascade IRQ7; repair PIC mask lifecycle and prove exact Xzed cursor movement |

X11 repair precedes `/dev/gpuN`; i915, Vulkan, GLES, and Wayland remain
separately extractable Phases.

## Goals

- Make the existing Xzed/zwm/zshell/zterm environment launch and accept correct
  evdev input.
- Provide a driver-independent zedBSD GPU UAPI, with i915 as the first native
  driver and safe takeover from the boot framebuffer.
- Provide the declared Vulkan profile, OpenGL ES 2.0 on Vulkan, and a Wayland
  desktop environment.

## WS completion conditions

WS007 is complete when X11 passes launch/input/session tests, `/dev/gpu0` and
`/bin/gpu` satisfy their versioned UAPI tests, i915 passes target-hardware
rendering and recovery tests, the declared Vulkan/GLES profiles pass their
applicable suites, and the Wayland desktop completes its lifecycle/input/client
acceptance cases.

## 1. Objective

Stabilize the existing X11 path, then add a zedBSD-native GPU interface that
abstracts a deliberately declared Vulkan-capable subset without copying Linux
DRM's driver-specific userspace interfaces. Implement i915 first, provide
OpenGL ES 2.0 on Vulkan, and build a Wayland compositor/desktop environment on
the resulting graphics and evdev stacks.

Compute shaders may be unsupported. Devices with an OpenGL ES 2.0-class feature
set may be supported through an explicitly reduced capability profile.

## 2. Existing framebuffer baseline

On amd64, `/dev/console` and `/dev/graphics` currently use the boot-time VBE/GOP
linear framebuffer through HAL-style interfaces. The GPU driver must be able to
take over scanout after initialization while preserving a fallback path.
Takeover must coordinate mappings, console rendering, ownership, modesetting,
panic output, suspend/resume if supported, and driver failure. It must not leave
stale mappings or a black console without a recorded fallback reason.

## 3. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| GFX-00 | Complete | Xzed/startx packaging and launch audit | Current X11 tree/image rules | `/bin/startx` is present and starts the session by command name in QEMU |
| GFX-01 | Carried forward | Xzed mouse coordinate/input repair | IN-00 complete; IN-01–IN-04 pending | Relative state/bounds pass; absolute path and original defect await a reproducer |
| GFX-02 | Planned | Xzed, zwm, zshell, and zterm integration regression suite | GFX-00/01 | Start, input, redraw, child exit, and clean session shutdown pass |
| GFX-10 | Proposed | Versioned `/dev/gpuN` UAPI and capability profiles | Memory/VFS/security audit, BR-00 GPU inventory | Review resolves object lifetime, synchronization, validation, and reduced-profile rules |
| GFX-11 | Proposed | GPU core and `/bin/gpu` diagnostic/control command | GFX-10 | UAPI conformance, invalid-request isolation, resource cleanup, and diagnostics pass |
| GFX-12 | Proposed | Console/graphics display takeover and fallback contract | GFX-10/11 | Handoff, failure fallback, and panic/console output pass without stale ownership |
| GFX-20 | Proposed | Initial i915 driver for the exact Latitude GPU | HW-00/HW-30, GFX-10–12 | Modeset/scanout/submission/recovery pass on hardware |
| GFX-30 | Proposed | Userspace Vulkan implementation for the declared zedBSD profile | GFX-11/20 | Published feature table and applicable conformance/smoke tests pass |
| GFX-31 | Proposed | OpenGL ES 2.0 implementation on Vulkan | GFX-30 | GLES2 API/rendering tests pass within documented limits |
| GFX-40 | Proposed | Wayland compositor and desktop environment | IN evdev, GFX-12/30 or software fallback, IPC/shared memory | Native clients, input, surfaces, session lifecycle, and recovery pass |

## 4. GPU UAPI design requirements

The stable, driver-independent UAPI must define objects and capabilities before
i915 details leak into it. At minimum, the design Phase evaluates:

- adapter discovery and immutable feature/limit queries;
- process/context isolation and object handles;
- buffer allocation, mapping, sharing, cache/domain transitions, and lifetime;
- images, samplers, shader modules, pipelines/descriptors, command recording or
  submission, queues, fences, and explicit errors;
- display/scanout objects and framebuffer handoff;
- validation of sizes, offsets, formats, shader input, and synchronization;
- process exit, GPU hang, reset, and resource reclamation;
- version negotiation and forward-compatible extension rules.

Calling the layer “Vulkan-like” does not imply full Vulkan. GFX-10 publishes a
capability profile that distinguishes mandatory graphics features, optional
features, explicitly unsupported compute, and the minimum GLES2-class profile.

## 5. i915 and QEMU boundary

The first target is the exact Intel GPU identified in the Latitude 5320. QEMU
can validate device-independent GPU core and UAPI logic using a test device or
software backend, but it should not be treated as a faithful Tiger Lake i915
model. PCI passthrough may be an optional development route if it can be used
safely. Final driver acceptance is physical-hardware evidence.

Required firmware blobs, display tables, stolen memory, GGTT/PPGTT, interrupt
handling, power wells, modesetting, submission, and reset are specified after
the target PCI ID and platform data are captured.

## 6. X11-first and Wayland sequencing

The current tree already contains a `startx` source/install path, so GFX-00 is an
image/package audit rather than an assumption that the script does not exist.
X11 repair provides an early evdev consumer and usable debugging environment.

Wayland work starts only after the input ABI and a scanout/rendering path are
stable. A software-rendered framebuffer backend may be retained for debugging
and unsupported GPUs, but accelerated completion requires the declared GPU
stack. The compositor must not depend on Linux DRM-specific UAPIs.

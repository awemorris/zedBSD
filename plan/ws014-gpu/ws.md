# WS014: native GPU stack

Last updated: 2026-08-27

WSID: `ws014`

Status: Blocked; future architecture discussion on manual hold

Parent: [master plan](../master.md)

Last verified Phase: none

Resume point: none until the user explicitly resumes GPU architecture
discussion. Do not transfer implementation ownership from WS004/WS007 or
publish `/dev/gpuN` UAPI while the manual hold is active.

Shared reviews: [WS014 review index](tests/README.md)

## Goals

- Define a versioned, driver-independent zedBSD GPU UAPI at `/dev/gpuN`.
- Implement i915 first without exposing i915-specific userspace ioctls.
- Provide a declared Vulkan profile and OpenGL ES 2.0 on that implementation.
- Allow a GPU driver to take over scanout from the boot framebuffer with safe
  console/graphics fallback.

## Objective

Separate the native GPU stack from the broader hardware and desktop WSs, then
resolve its object, memory, submission, synchronization, display, security, and
capability contracts before implementation begins.

## Scope

- `/dev/gpuN` versioning, discovery, handles, contexts, queues, memory/images,
  synchronization, validation, errors, teardown, and permissions;
- mandatory graphics and reduced GLES2-class profiles, with compute optional;
- userspace Vulkan and GLES layering over the kernel UAPI;
- `/bin/gpu` diagnostics;
- boot framebuffer, `/dev/console`, and `/dev/graphics` provider takeover;
- i915 ownership and its WS004 PCIe/DMA/MSI/firmware dependencies;
- later WS007 X11/Wayland consumers and WS008 BeUI consumers.

## Non-goals

- Linux DRM or driver-specific ioctl compatibility;
- claiming full Vulkan before a profile and conformance boundary are published;
- requiring compute shaders on every supported GPU;
- implementing GPU code during the discussion Phase.

## Dependencies

- [WS004](../ws004-hardware/ws.md) owns reusable PCIe, DMA, interrupt, power,
  and firmware foundations.
- [WS007](../ws007-graphics/ws.md) owns X11 and Wayland desktop integration.
- WS006 owns evdev input; WS008 owns Noct/BeUI; WS009 owns public references.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws014-p001` | [GPU architecture discussion](phase001-architecture-discussion/phase.md) | Blocked by manual hold | Resume later to freeze UAPI/profile/display/i915 boundaries and the Phase map |

No implementation Phase is defined until `ws014-p001` is complete.

## Confirmed product direction

- `/dev/gpu0` is the first instance of `/dev/gpuN`.
- The public interface is driver-independent and only thinly exposes the
  primitives needed by the declared Vulkan-like graphics model.
- Compute may be unsupported; GLES2-class hardware may use a reduced profile.
- `/bin/gpu` communicates with the device through the public UAPI.
- GPU initialization may replace the VBE/GOP linear-framebuffer provider, but
  fallback and panic output must remain defined.
- i915 for the Latitude 5320 is the first hardware target.

## WS completion direction

The current planning-stage WS may pause after p001 produces fixed architecture
and implementation Phase decomposition. UAPI or driver completion conditions
are deferred until the capability profile and acceptance environments exist.

## Reconsideration boundaries

Reconsider if the common UAPI requires driver-private command formats, cannot
validate untrusted submissions, cannot recover resources on process exit or
GPU reset, or cannot preserve console output during display takeover failure.

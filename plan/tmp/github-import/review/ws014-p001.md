# WS014 Phase 001: GPU architecture discussion

Last updated: 2026-08-27

WSID: `ws014`

Phase ID: `p001`

Combined ID: `ws014-p001`

Status: Blocked (manual hold)

Parent: [WS014](../ws.md)

Reviews: [WS014 review index](../tests/README.md)

## Objective

Define the stable boundary beneath userspace Vulkan/GLES and above native GPU
drivers, together with display takeover and the first i915 target, before any
public UAPI or implementation Phase is selected.

## Baseline

WS004 already owns PCIe/DMA/MSI foundations and proposes i915 hardware work.
WS007 currently records `/dev/gpuN`, i915, Vulkan, GLES, and Wayland ideas in
one broad graphics/desktop WS. amd64 console and graphics currently use the
boot VBE/GOP linear framebuffer.

## Scope

- device discovery, version negotiation, capability profiles, and limits;
- context/process isolation, handles, buffers, images, mapping/sharing, cache
  transitions, queues, submissions, shaders/pipelines, and fences;
- validation, malformed input, hang/reset, detach, and process-exit cleanup;
- display ownership, modes, scanout, bootfb takeover, fallback, and panic path;
- kernel UAPI versus userspace Vulkan/GLES responsibility;
- `/bin/gpu` purpose and permissions;
- first Tiger Lake/i915 boundary, firmware, test double, and hardware evidence;
- transfer of responsibilities from WS004 and WS007 and later Phase ordering.

## Non-goals

- implementing a Linux DRM clone;
- copying Vulkan wholesale into the kernel ABI;
- promising compute, full Vulkan, or exact i915 hardware behavior before the
  declared profile and target inventory are fixed;
- source-code changes in this Phase.

## Open decisions

- Mandatory graphics profile and optional extensions, including compute.
- Shader input/validation boundary and whether any portable IR is UAPI.
- Buffer/image sharing, coherency, address-space, queue, and fence semantics.
- Single node with display-master rights versus separately permissioned roles.
- Atomic display-provider takeover and fallback ownership among GPU,
  `/dev/console`, and `/dev/graphics`.
- i915 kernel/user split, firmware policy, reset model, and safe physical gates.
- Exact WS004/WS007 responsibility transfer without renumbering completed
  Phases.

## Work packages

- [ ] Freeze the layer diagram and kernel/userspace/driver responsibilities.
- [ ] Freeze UAPI object lifetime, capability, memory, queue, sync, and error
      requirements.
- [ ] Freeze the reduced profile and optional compute policy.
- [ ] Freeze display takeover, fallback, panic, suspend/reset, and permissions.
- [ ] Freeze the first i915 target inputs, firmware, and evidence model.
- [ ] Decide WS004/WS007 transfer and downstream WS006/WS008/WS009 handoffs.
- [ ] Split implementation into bounded UAPI, core, display, i915, Vulkan,
      GLES, diagnostic, and integration Phases.

## Acceptance

Every review case in the [WS014 review index](../tests/README.md) has an
explicit decision, security/failure semantics, owner WS, and later acceptance
environment. This Phase makes no runtime claim.

## Actual results and evidence

The existing plans establish the desired product direction but leave the
kernel/userspace abstraction, security validation, display ownership, and
cross-WS ownership open. Discussion remains in progress outside any Queue.

## Interruption / resumption

Detailed GPU design is intentionally deferred. Resume only after the user
explicitly selects this discussion, beginning with mandatory/reduced capability
profiles and object lifetime. Do not Queue UAPI or i915 code first.

## Remaining debt and handoff

All code, fixtures, QEMU models, physical i915 validation, Vulkan/GLES work,
desktop integration, and documentation remain later extracted Phases.

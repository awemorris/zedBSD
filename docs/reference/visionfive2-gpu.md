VisionFive 2 GPU (PowerVR BXE-4-32)
===================================

Status: reference. What the VisionFive 2's GPU offers to Vulkan programs, the
firmware it needs, how the driver reports its start-up, and the test programs
that check it.

## Devices

The StarFive JH7110 has two GPU nodes:

- The display node: the display controller (DC8200). It only shows images and
  has no Vulkan device.
- The render node: the Imagination PowerVR BXE-4-32. This is the Vulkan
  device.

The driver registers the display node first, so the display node is normally
`/dev/gpu0` and the render node `/dev/gpu1`. The numbers are the lowest free
numbers in probe order, not fixed names: without the firmware the render node
does not appear, and a board with another GPU takes numbers too. A program
tells the nodes apart by the roles that `GPU_DEVICE_QUERY` reports, not by the
number. The rules for node names and permissions, and the request interface of
both nodes, are in [GPU device interface](gpu.md); what each node offers is in
[GPU drivers and node kinds](gpu-drivers.md).

`libvulkan` opens both nodes and offers one physical device, the render node.
A program that presents to the screen through `VK_KHR_display` draws on the
render node. The library then copies each finished frame to the display
node.

## Supported Vulkan

- **Version:** Vulkan 1.0. An instance that asks for a later version is
  refused with `VK_ERROR_INCOMPATIBLE_DRIVER`.
- **Queues:** one queue family, with `GRAPHICS`, `COMPUTE` and `TRANSFER`.
- **Samples and layers:** 1 sample only, for every attachment and sampled
  image. Framebuffers have 1 layer.
- **Tiling:** images with `VK_IMAGE_TILING_LINEAR` cannot be sampled. Use
  `OPTIMAL` tiling for anything a shader reads.
- **Formats:**

| Format | Use |
| --- | --- |
| `B8G8R8A8_UNORM`, `B8G8R8A8_SRGB`, `R8G8B8A8_UNORM`, `R8G8B8A8_SRGB` | colour attachment with blending, sampled with linear filtering, transfer |
| `R8_UNORM`, `R8G8_UNORM` | sampled, transfer |
| `D16_UNORM`, `D32_SFLOAT` | depth attachment |
| `R32_SFLOAT`, `R32G32_SFLOAT`, `R32G32B32A32_SFLOAT` | vertex attribute; sampled, transfer |
| `R32G32B32_SFLOAT` | vertex attribute only |
| `R16G16B16A16_SFLOAT` | colour attachment, sampled, transfer |

## Memory

Every host-visible memory type is also `HOST_COHERENT`, so no flush or
invalidate is needed.

- The processor reaches GPU memory only through uncached mappings.
- Buffers the processor writes or reads also bypass the GPU's system-level
  cache.
- What one side writes, the other side sees once the fence of the submission
  has signalled.

## Firmware and its licence

The driver boots Imagination's firmware `rogue_36.50.54.182_v1.fw` (version
1.1.OS@6976702). The image installs it as
`/lib/firmware/powervr/rogue_36.50.54.182_v1.fw`.

The firmware is distributed under Imagination's `LICENSE.powervr`, which
forbids reverse engineering, decompilation and disassembly. For that reason
it is a separate optional package, `powervr-firmware`
([userland/firmware/powervr](../../userland/firmware/powervr/Makefile)).

- The first build that fetches it shows the licence and asks whether you
  agree.
- The image keeps a copy of the licence in
  `/usr/share/licenses/powervr-firmware/`.

Without the firmware, the display node still works but the render node does
not appear.

## Start-up stages

As the driver brings the GPU up, it writes one kernel log line for each
stage:

    powervr: <stage>: ok
    powervr: <stage>: fail(<reason> [value])

- **Stages:** `firmware`, `render-node`, `compute`, `ta`, `3d`, `transfer`,
  `present`.
- **When lines appear:** a stage's `ok` is logged the first time it
  succeeds. A failing stage logs three times at most, then
  `powervr: <stage>: further failures not shown`.
- **Reasons:** `error`, `enoent`, `timeout`, `bad image`, `ebusy`,
  `coherency`, `fault`, `spirv subset`, `free list`, `format`, `import`,
  `stride`, `claim` and `reset`.
- **Reset values:** a `reset` failure also carries the firmware's own reason,
  for example `lockup` or `overrun`.

The sysctl `hw.gpu.powervr.stages` gives the current state of every stage on
one line:

    firmware=ok render-node=ok compute=ok ta=ok 3d=ok transfer=ok present=-

`-` means the stage has not run yet. `fail:<reason>` means its latest attempt
failed. A later success turns it back to `ok`.

## GPU reset

The driver recovers from a submission that never finishes:

- **The stuck submission:** it is abandoned after two seconds, and the
  program gets `VK_ERROR_DEVICE_LOST` for it.
- **Firmware reset:** when the firmware resets the stuck context, the kernel
  log shows `powervr: context reset notice: …`.
- **Isolation and restart:** when the firmware cannot clean the context up,
  the session is isolated. The next open of the render node restarts the
  firmware, provided no other program has the node open.
- **Afterwards:** new instances and devices work again. The objects of the
  lost device are never revived.

## Test programs

All three programs are in the VisionFive 2 test images, not in the ordinary
image.

### gpu-selftest

[userland/tests/gpu-selftest](../../userland/tests/gpu-selftest/main.c) runs
the stages `kernel`, `node`, `instance`, `device`, `compute`, `image`,
`clear`, `copy`, `draw`, `sample`, `depth`, `present-clear` and `present` in
that order. Each stage runs in a child process with a time limit, and the
program prints one line for each:

    gpu-selftest: <stage>: ok
    gpu-selftest: <stage>: fail(<reason>)
    gpu-selftest: <stage>: skip(<stage it needs>)
    gpu-selftest: 13 ok, 0 fail, 0 skip

- **The `kernel` stage:** copies `hw.gpu.powervr.stages`.
- **The two `present` stages:** show frames on the screen.
- **Exit status:** 0 when no stage failed.

### vkmatrix

[userland/tests/vkmatrix](../../userland/tests/vkmatrix/main.c) checks the
parts of Vulkan the Keiland desktop draws with. Each case renders a 64×64
image, reads it back and compares every pixel with the image the processor
works out:

- push constants of 4, 32 and 128 bytes
- the compositor's vertex layout
- nearest and linear sampling
- two descriptor sets
- premultiplied and additive blending
- 32 and 64 draws in one pass
- a loaded second pass
- dynamic scissor and viewport
- sampling a B8G8R8A8 texture
- sampling a rendered image
- copies of 64-bit and 128-bit texels (`copy-64`, `copy-128`)
- clears of 64-bit and 128-bit images read back (`clear-64`, `clear-128`)
- rendering to `R16G16B16A16_SFLOAT` (`render-f16`)

The output is one line for each case and a total:

    vkmatrix: <case>: PASS
    vkmatrix: <case>: FAIL(<call> <VkResult> | pixel (<x>,<y>) 0x<seen>, expected 0x<wanted> | <n> bad, words 0x<mask>, (<x>,<y>) <seen>/<wanted> ...)
    vkmatrix: 20/20 passed

- **One case:** an argument runs only that case.

### vkreset

[userland/tests/vkreset](../../userland/tests/vkreset/main.c) checks recovery
from a GPU reset.

**It resets the GPU.** Run it only as `vkreset --isolated`, when no other
program has the GPU open (no desktop session, no other test). Without the
flag it does nothing and exits with 2.

It runs four steps:

1. **hang:** submits a compute kernel that never ends. The step passes when
   the submission or its fence reports `VK_ERROR_DEVICE_LOST`.
2. **close:** destroys the lost device.
3. **compute:** a new instance and device run a compute fill of 4096 words.
4. **clear:** another new device clears a 64×64 image and reads it back.

The output looks like this:

    vkreset: hang: PASS (<ms> ms, <call> VK_ERROR_DEVICE_LOST)
    vkreset: close: PASS (<ms> ms)
    vkreset: compute: PASS (<ms> ms, 4096/4096 words)
    vkreset: clear: PASS (<ms> ms, 4096/4096 pixels)
    vkreset: PASS

The exit status is 0 only on `PASS`. After it, `hw.gpu.powervr.stages` shows
`compute=fail:timeout` until the next successful compute submission.

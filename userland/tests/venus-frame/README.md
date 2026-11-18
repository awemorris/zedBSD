# Venus frame diagnostic

`venus-frame` is an original zedBSD userspace client for a finite subset of
Vulkan commands over Venus wire format 1. It uses the ordinary `/dev/gpuN`
interface. It is not `libvulkan.so`, a Vulkan ICD, or a claim of Vulkan
conformance. No Mesa or virglrenderer implementation is included in this
program or linked into the zedBSD base system.

```
venus-frame --phase=2d --frame=1 --hold=120
venus-frame --phase=venus --frame=1 --hold=120
venus-frame --phase=venus --frame=2 --hold=120
```

An optional `--device=/dev/gpuN` selects a different device. `--hold` accepts
0 through 120 seconds and defaults to 10. Frame numbers are decimal integers
from 0 through 1000000. The default phase is `venus`, and the default frame is 1.

The displayed image is 256 by 192 pixels, split at x=128. Odd frame numbers
produce red `(255,0,0)` on the left and green `(0,255,0)` on the right. Even
numbers produce blue `(0,0,255)` on the left and yellow `(255,255,0)` on the
right. Every alpha byte is 255. Adjacent frame numbers therefore require
visibly different captures.

The `2d` phase fills the pixels on the CPU and tests storage, transfer, and
scanout. It does not constitute Venus or Vulkan success. The `venus` phase
creates a Vulkan instance, physical/logical device, graphics queue, an optimal
RGBA8 image, a coherent host-visible readback buffer, command pool/buffer, and
fence. Queue registration uses `vkGetDeviceQueue2` with a
`VkDeviceQueueTimelineInfoMESA` chain assigning timeline 1; renderer 1.1.0
rejects the older `vkGetDeviceQueue` command. Two `vkCmdClearColorImage` operations and two
`vkCmdCopyImageToBuffer` operations produce the two bands. Image barriers order
the clears and copies; a final buffer barrier makes transfer writes visible
to host reads. The program waits for the actual Vulkan fence before exporting
the readback allocation as a HOST3D blob.

Only the bytes read from that completed Vulkan allocation are validated and
copied to the storage resource used for scanout. A mismatch fails immediately;
the program never substitutes a CPU-rendered image in the `venus` path. This is
an initial copy-based presentation path, not a zero-copy Vulkan swapchain.
There are no shaders, graphics pipelines, SPIR-V compilation, Vulkan WSI
extensions, or full Vulkan API dispatch in this diagnostic. The initial amd64
loop uses an 8 MiB host-visible aperture within the existing device-mapping
window; larger aperture support is outside this finite implementation.

After every pixel passes comparison, the program prints a 32-bit FNV-1a hash
of all 196608 RGBA bytes. After `GPU_PRESENT` succeeds it prints:

```
VENUS-FRAME PRESENT phase=venus frame=1 left=255,0,0 right=0,255,0
```

The automation must additionally capture the actual QEMU display and compare
its pixels. This serial marker by itself is not display evidence. The session
remains open during the hold period; closing it releases its display ownership,
GPU resources, and renderer context, including the context's Vulkan objects.

Each protocol request includes a `vkSetReplyCommandStreamMESA` prefix and a
`vkSeekReplyCommandStreamMESA` / `vkEnumerateInstanceVersion` trailer. The final
API-version word is cleared before submission and polled separately. Only
after it changes does the program copy and decode the response body. This
avoids treating virtqueue receipt as completion in a QEMU render-server
configuration. Each poll loop is bounded to 1000 attempts separated by 10 ms;
system-call transport delays may add to that duration. Vulkan fence completion
is checked independently with `vkGetFenceStatus`. Unsupported blocking queue
waits and memory mapping commands are never serialized.

## Protocol provenance

The command numbers, Vulkan structure tags, field order, handle identities,
pointer/array counts, union tags, and primitive encodings were independently
implemented from these public interface descriptions and checked against the
host renderer's matching protocol revision:

- [Venus serialization specification, revision ca19b6358d7cc491bc3e4de76f04c6700876a8fa](https://gitlab.freedesktop.org/virgl/venus-protocol/-/blob/ca19b6358d7cc491bc3e4de76f04c6700876a8fa/docs/VK_EXT_command_serialization.txt)
- [Stable command IDs at that revision](https://gitlab.freedesktop.org/virgl/venus-protocol/-/blob/ca19b6358d7cc491bc3e4de76f04c6700876a8fa/xmls/VK_EXT_command_serialization.xml)
- [MESA transport interfaces at that revision](https://gitlab.freedesktop.org/virgl/venus-protocol/-/blob/ca19b6358d7cc491bc3e4de76f04c6700876a8fa/xmls/VK_MESA_venus_protocol.xml)
- [virglrenderer 1.1.0, commit 1aeaf5e10a9c89096e96d09599aa419d5c50712f](https://gitlab.freedesktop.org/virgl/virglrenderer/-/tree/1aeaf5e10a9c89096e96d09599aa419d5c50712f/src/venus), whose embedded protocol identifies itself as `git-ca1e9220`, wire format 1, Vulkan XML 1.3.269.
- [Mesa's Venus architecture documentation](https://docs.mesa3d.org/drivers/venus.html)

Upstream source was used as an interoperability reference, not copied as
implementation. The program needs only zedBSD libc and `uapi/gpu.h`; it does
not need upstream headers, a generator, an external runtime, or a build-time
network download. The tested host version and actual guest/display evidence
belong in the WS014 phase results, not in an unconditional claim here.

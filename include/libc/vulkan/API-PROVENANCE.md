# Vulkan declaration provenance

These are public API declarations, not imported Vulkan implementation code.
The ABI and numeric declarations are selected from the Khronos Vulkan
1.3.269 declarations:

- Khronos Vulkan-Headers tag `v1.3.269`.
- Input path: `include/vulkan/vulkan_core.h`.
- Input SHA-256: `8cb01233ecf0fac130f0db2fdbbd79b6643f91f65c9afb807978924148798287`.
- [Pinned source](https://github.com/KhronosGroup/Vulkan-Headers/blob/v1.3.269/include/vulkan/vulkan_core.h).
- Declaration license: Apache-2.0, copyright 2015–2023 The Khronos Group Inc.
- The derived headers (`vulkan_core.h`, `vulkan_wayland.h`, `vulkan_external.h`,
  `vulkan_video.h`, `vulkan_display_control.h`) carry the zedBSD copyright in the
  file header and, in a separate comment block below it, the Khronos copyright and
  the Apache-2.0 license of the declarations they contain. The `vk_video/` headers
  are unmodified Khronos files and carry only the Khronos notice.
- The copy virglrenderer 1.1.0 (commit `1aeaf5e10a9c89096e96d09599aa419d5c50712f`)
  carries as `src/venus/venus-protocol/vulkan_core.h` is the same file, byte for
  byte (the SHA-256 above); it is not a Venus declaration.
  The independent zedBSD selection/formatting tool and platform wrapper are Zlib.

The maintained header exposes 137 Vulkan 1.0 commands and 18 Vulkan-1.0-era
commands from KHR_surface, KHR_display, KHR_swapchain and KHR_display_swapchain.
Later device-group swapchain commands and their structures are excluded.
Numeric constants occurring in shared registry enum types remain available as
declaration data; they do not advertise corresponding runtime support. There
are no `VK_VERSION_1_1`, `VK_VERSION_1_2` or `VK_VERSION_1_3` feature guards.
The library's extension enumeration and enabled feature contract decide support.

No production build generates this header. From the repository root, an
intentional maintenance update uses the independent Noct tool:

```sh
timeout 90 build/NoctLang/build-static/noct \
  userland/desktop/libvulkan/tools/maintain-api.noct \
  /path/to/verified/pinned/vulkan_core.h \
  /tmp/vulkan_core.h
```

First verify the input hash above, compare the generated output against the
maintained header, and run the ABI checks before replacing it. The tool's source
is maintained in this repository; no upstream generator or marshaler is copied.
Changing the API-data revision or function set requires reviewing this record,
the public ABI and the supported runtime scope together.


## Wayland declaration addition

`vulkan_wayland.h` adds the two commands and the creation record of
`VK_KHR_wayland_surface`, revision 6. The declaration data was checked against
[Khronos Vulkan-Headers v1.3.269](https://github.com/KhronosGroup/Vulkan-Headers/blob/v1.3.269/include/vulkan/vulkan_wayland.h),
SHA-256 `3728578b8d6d98f6f3d20672406f869253eeacae8546a9d9577bca5c63a88d12`.
The declaration license remains Apache-2.0. The platform wrapper includes it
when `VK_USE_PLATFORM_WAYLAND_KHR` is defined; direct inclusion is also supported.

The core/direct-display selection remains 155 commands; the complete public
library now contains 157 commands. The independent dispatch maintenance tool
reads the separate maintained Wayland header in addition to `vulkan_core.h`.
The extra private external-image/memory chain encoding is implementation code,
not a public advertisement of external-memory extensions or a guest dma-buf ABI.
The [official Wayland extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_wayland_surface.html)
requires MAILBOX support as well as the implemented FIFO path.

## External allocation and fence declarations

`vulkan_external.h` selects additional declaration data from the same pinned
`vulkan_core.h` and input hash above. The independent Noct maintenance tool is
`userland/desktop/libvulkan/tools/maintain-external.noct`; it accepts the verified
input path and an output path. The declaration license remains Apache-2.0.

The complete public command set is now 170: the existing 157 commands plus seven
`VK_KHR_get_physical_device_properties2` commands, one
`VK_KHR_external_memory_capabilities` command, two
`VK_KHR_external_memory_fd` commands, one
`VK_KHR_external_fence_capabilities` command, and two
`VK_KHR_external_fence_fd` commands. `VK_KHR_external_memory` and
`VK_KHR_external_fence` contribute declarations and dependencies, but no commands.
Core Vulkan remains 1.0; adding these KHR entry points does not advertise 1.1.

The guest public handle type is `OPAQUE_FD`. It retains a zedBSD kernel object
and does not expose a Linux dma-buf or sync-file ABI. The internal Venus
allocation export/import protocol remains a backend implementation detail.

Stock renderer contexts translate public OPAQUE memory to native DMA_BUF and
report only the native buffer/image profiles that actually support that type.
An optional paired proxy/server renderer negotiates native OPAQUE allocation
sharing through an exact 168-byte Venus capset: little-endian magic
`0x5a424453` at offset 160 and exactly flags `1` at offset 164. Unknown lengths,
magic or flag bits retain the stock profile. This is an isolated renderer
extension, not an upstream Venus capability or an additional Vulkan command.

A negotiated context uses native OPAQUE consistently for public allocation,
buffer/image creation and external capability queries. Native OPAQUE blobs are
SHAREABLE; public host-visible allocations additionally request MAPPABLE.
Explicit private WSI images continue using native DMA_BUF with CROSS_DEVICE,
including on negotiated contexts. No new Vulkan core version or dedicated
allocation extension is advertised. Dedicated-only native profiles return no
external buffer capability and `VK_ERROR_FORMAT_NOT_SUPPORTED` for external
image queries. Actual extension enumeration remains conditional on the
corresponding kernel capabilities.

Memory import checks immutable size, memory type, schema, native device UUID
and a stable zedBSD-specific driver UUID before publishing a new memory object.
The paired renderer also retains its native allocation size, memory type and
unmodified device/driver UUIDs with the host OPAQUE fd, checking those values
before native import. A page-rounded native allocation keeps its original
public byte bounds after import. Each host or guest import retains a distinct
owned reference; original allocation destruction does not consume exported fds.

Fence import/export advertises only reference-bearing OPAQUE_FD; SYNC_FD and
other handle types have zero external-fence capabilities and are rejected by
the import/export entry points. Each export creates a close-on-exec descriptor.
Temporary import hides the permanent payload, and reset restores and unsignals
the permanent payload. A shared successful signal requires actual native fence
completion, while producer loss terminates pending ownership with an error.

The [standard OPAQUE fence compatibility rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalFenceHandleTypeFlagBits.html)
require matching Vulkan device and driver UUIDs. The present kernel fence
payload validates its immutable registered GPU identity and typed-handle ABI;
it does not carry or compare the two Vulkan UUID arrays. Thus the currently
accepted one-native-GPU configuration has a checked kernel identity boundary,
while an additional defensive UUID check for multiple native physical devices
behind one kernel GPU remains outside this implementation. Applications must
still satisfy the standard UUID compatibility requirement; this limitation
must not be described as an implemented fence UUID comparison.

`vkGetMemoryFdPropertiesKHR` has no valid OPAQUE_FD query:
[VUID-vkGetMemoryFdPropertiesKHR-handleType-00674](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetMemoryFdPropertiesKHR.html)
excludes that handle type. The exported function therefore rejects the presently
unsupported query types; allocation import instead validates the immutable
exported size, memory type and UUID metadata. Successful standard imports consume
their input descriptor, while failed imports leave it owned by the caller.

The expanded ABI fixture checks 148 structures, 942 fields and 2394 enum constants
against independently compiled ILP32/LP64 declarations. The full 170-command
dispatch fixture checks extension gating against its independent command list.

## Memory-requirement and dedicated-allocation declarations

`vulkan_external.h` additionally selects `VK_KHR_get_memory_requirements2` and
`VK_KHR_dedicated_allocation` from the same pinned `vulkan_core.h` (input SHA-256
above, fetched again from the pinned commit and verified on 2026-09-30), with
the 1.1 structures they alias: `VkBufferMemoryRequirementsInfo2`,
`VkImageMemoryRequirementsInfo2`, `VkImageSparseMemoryRequirementsInfo2`,
`VkMemoryRequirements2`, `VkSparseImageMemoryRequirements2`,
`VkMemoryDedicatedRequirements` and `VkMemoryDedicatedAllocateInfo`. The tool
change is the extension and type lists of `maintain-external.noct`; the output
differs from the previous header only by these additions and its first comment
line.

The public command set is now 173: the three `VK_KHR_get_memory_requirements2`
commands (`vkGetBufferMemoryRequirements2KHR`, `vkGetImageMemoryRequirements2KHR`,
`vkGetImageSparseMemoryRequirements2KHR`). `VK_KHR_dedicated_allocation` adds no
command. `maintain-dispatch.noct` regenerated `dispatch-table.inc` and
`api-commands.tsv` with these records. The three commands are answered in
the library from the 1.0 queries and need no wire identifier. Core Vulkan
remains 1.0.

## The command numbers

The numbers of the Vulkan commands in the stream libvulkan submits are the zedBSD
GPU command protocol's, declared in `include/uapi/gpu-op.h` with zedBSD's own
names (`GPU_OP_CREATE_INSTANCE` and so on), which libvulkan, the i915 Vulkan
executor and `venus-frame` share. Version 1 reuses the numbers of the Venus
protocol's wire format 1 (virglrenderer 1.1.0) for the same Vulkan commands, so
that a Venus renderer on a virtual machine's host accepts the stream; the file,
its names and later numbers are zedBSD's. No virglrenderer file is read to make
it, and libvulkan no longer carries `opcodes.h` or `LICENSE-PROTOCOL`.

## Vulkan Video and synchronization2 declarations

`vulkan_video.h` selects declaration data for `VK_KHR_video_queue` (revision 8),
`VK_KHR_video_decode_queue` (8), `VK_KHR_video_decode_h264` (9),
`VK_KHR_video_decode_h265` (8) and `VK_KHR_synchronization2` (1), with the Vulkan 1.3 types synchronization2's names
alias (`VkFlags64`, the stage and access flags and constants, `VkSubmitFlagBits`,
the barrier, dependency, submit and feature structures). The input is a different
revision from the core selection above, because the pinned 1.3.269 input is not
available to the maintainers; the selected extensions' declarations have not
changed since their final revisions:

- Khronos Vulkan-Headers 1.4.309, as packaged by Debian (`libvulkan-dev`
  1.4.309.0-1): `/usr/include/vulkan/vulkan_core.h`, SHA-256
  `39f358bb99f7be5206524d119de4a5b5ab5c515c329c2857ff395d37e6387b4c`.
- The H.264 standard headers are copied unchanged from the same package into
  `vk_video/` (std header `VK_STD_vulkan_video_codec_h264_decode` 1.0.0):
  `vulkan_video_codecs_common.h` `7afc98e84d3bea72e40a65161463abbd4be2ff1e1853eca3f8c331afb7329a8b`,
  `vulkan_video_codec_h264std.h` `05859b58e094a8dcc88cb3766a1fcbfd62bcaab20c93d2c3e5cec853628f277c`,
  `vulkan_video_codec_h264std_decode.h` `d2be01f84f773644a24b7634f49e63ff8c6f0e807119b1bf609176585031f93a`.
- The H.265 standard headers are copied unchanged from the same package into
  `vk_video/` (std header `VK_STD_vulkan_video_codec_h265_decode` 1.0.0; the
  `VK_KHR_video_decode_h265` block includes them):
  `vulkan_video_codec_h265std.h` `a5d0b2e7b47843db610d2769c7b0202c90cd7db82108b60fb2631d1c7976a395`,
  `vulkan_video_codec_h265std_decode.h` `944646ced5bf9f49b79144c18369f0b932900c375fd81c262a3c30e9c60de8c6`.
- Declaration license: Apache-2.0, copyright 2015–2025 The Khronos Group Inc.
  (`LICENSE-API`). The independent selection tool `tools/maintain-video.noct` is Zlib.

```sh
timeout 90 build/NoctLang/build-static/noct \
  userland/desktop/libvulkan/tools/maintain-video.noct \
  /usr/include/vulkan/vulkan_core.h include/libc/vulkan/vulkan_video.h
```

`vulkan.h` includes `vulkan_video.h`. The declarations do not advertise support:
libvulkan offers these extensions only on a backend whose capset declares video
decode of the codec (a native renderer), and it names Vulkan 1.0 there, which the registry's
dependencies of these extensions do not allow. The libvulkan README lists every
such point.

The Khronos files keep their line comments, so a translation unit that includes
`vulkan.h` needs C99 or GNU89.  `tools/maintain-dispatch.noct` reads this header
too: the 13 video and 6 synchronization2 commands bring the dispatch table from
173 to 192 records (`VK_KHR_video_decode_h265` has no command).  The video structures are encoded by hand in
`userland/desktop/libvulkan/video.c` (not by `tools/maintain-codec.noct`, whose
input stays the core selection), because their chains, their bit-field flags and
the H.264 two-dimensional scaling lists need rules of their own.

## Display-control and surface-counter declarations

`vulkan_display_control.h` selects the declaration data of `VK_EXT_display_surface_counter`
(revision 1) and `VK_EXT_display_control` (revision 1) verbatim from the same input as
the video selection above: Khronos Vulkan-Headers 1.4.309 as packaged by Debian
(`libvulkan-dev` 1.4.309.0-1), `/usr/include/vulkan/vulkan_core.h`, SHA-256
`39f358bb99f7be5206524d119de4a5b5ab5c515c329c2857ff395d37e6387b4c`. The declarations of
both extensions are unchanged since their first revisions; the structure-type values
they use were already present in the core selection. Declaration license: Apache-2.0,
copyright 2015–2025 The Khronos Group Inc. (`LICENSE-API`). The independent selection
tool `tools/maintain-display.noct` is Zlib.

```sh
timeout 90 build/NoctLang/build-static/noct \
  userland/desktop/libvulkan/tools/maintain-display.noct \
  /usr/include/vulkan/vulkan_core.h include/libc/vulkan/vulkan_display_control.h
```

`vulkan.h` includes `vulkan_display_control.h`, which keeps the Khronos line comments
(C99 or GNU89, as for the video header). `tools/maintain-dispatch.noct` reads it too:
the five commands (`vkGetPhysicalDeviceSurfaceCapabilities2EXT`,
`vkDisplayPowerControlEXT`, `vkRegisterDeviceEventEXT`, `vkRegisterDisplayEventEXT`,
`vkGetSwapchainCounterEXT`) bring the dispatch table from 192 to 197 records. The
instance extension is always offered; the device extension only on a renderer whose
node has the display, topology-event and display-control capabilities. Core Vulkan
remains 1.0.

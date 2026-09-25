# Vulkan declaration provenance

These are public API declarations, not imported Vulkan implementation code.
The ABI and numeric declarations are selected from Khronos registry-derived
Vulkan 1.3.269 declaration data embedded in the pinned Venus protocol tree:

- virglrenderer tag `1.1.0`, commit `1aeaf5e10a9c89096e96d09599aa419d5c50712f`.
- Input path: `src/venus/venus-protocol/vulkan_core.h`.
- Input SHA-256: `8cb01233ecf0fac130f0db2fdbbd79b6643f91f65c9afb807978924148798287`.
- [Pinned source](https://gitlab.freedesktop.org/virgl/virglrenderer/-/blob/1aeaf5e10a9c89096e96d09599aa419d5c50712f/src/venus/venus-protocol/vulkan_core.h).
- Declaration license: Apache-2.0, copyright 2015–2023 The Khronos Group Inc.
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
  userland/base/libvulkan/tools/maintain-api.noct \
  /path/to/verified/pinned/vulkan_core.h \
  /tmp/vulkan_core.h
```

First verify the input hash above, compare the generated output against the
maintained header, and run the ABI checks before replacing it. The tool's source
is maintained in this repository; no upstream generator or marshaler is copied.
Changing the API-data revision or function set requires reviewing this record,
the public ABI and the supported runtime scope together.


## Wayland declaration addition (q309)

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
requires MAILBOX support as well as the implemented FIFO path. Actual acceptance
and limitations are recorded with WS014 p006, rather than changing WS030's closed
historical clearance.

## External allocation and fence declarations (q310)

`vulkan_external.h` selects additional declaration data from the same pinned
`vulkan_core.h` and input hash above. The independent Noct maintenance tool is
`userland/base/libvulkan/tools/maintain-external.noct`; it accepts the verified
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
Runtime acceptance and remaining limitations belong to WS014 p007.

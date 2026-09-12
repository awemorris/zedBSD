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

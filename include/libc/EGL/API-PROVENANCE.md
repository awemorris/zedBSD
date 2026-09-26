# EGL and OpenGL ES declaration provenance (WS068 p002)

These are the Khronos Group's public API headers, copied unchanged.  They are
declarations, not implementation code; zedBSD's libEGL, libGLESv2 and
libwayland-egl are independent implementations (userland/base/libegl,
userland/base/libglesv2, userland/base/libwayland-egl).

| Header | Registry, commit | SHA-256 | License |
| --- | --- | --- | --- |
| `EGL/egl.h` | KhronosGroup/EGL-Registry `db3425b8246136faccb5e2782b5694960bd6edf1` `api/EGL/egl.h` | `a7c24c828bf2e1a4dfe6511b4f4812b948a285eb54787074015896976d4da076` | Apache-2.0 |
| `EGL/eglext.h` | KhronosGroup/EGL-Registry `db3425b8246136faccb5e2782b5694960bd6edf1` `api/EGL/eglext.h` | `a5f574a0074001400c6c50232235ee00e9a67b2168bfd1c364ba0f6f52e5a75c` | Apache-2.0 |
| `EGL/eglplatform.h` | KhronosGroup/EGL-Registry `db3425b8246136faccb5e2782b5694960bd6edf1` `api/EGL/eglplatform.h` | `25b5391655effcf363a38fa00c3154f356a3eab3d35dc067847875513cf1e441` | Apache-2.0 |
| `KHR/khrplatform.h` | KhronosGroup/EGL-Registry `db3425b8246136faccb5e2782b5694960bd6edf1` `api/KHR/khrplatform.h` | `7b1e01aaa7ad8f6fc34b5c7bdf79ebf5189bb09e2c4d2e79fc5d350623d11e83` | MIT-style (Khronos, see the header) |
| `GLES2/gl2.h` | KhronosGroup/OpenGL-Registry `1cdd228e34966dd6b95bd203e9f84faba0f371a1` `api/GLES2/gl2.h` | `ba5e8e1755642efeb2d0f34b4cfe261cdbf6471e7763a40c7de10dff4c3c4921` | MIT |
| `GLES2/gl2ext.h` | KhronosGroup/OpenGL-Registry `1cdd228e34966dd6b95bd203e9f84faba0f371a1` `api/GLES2/gl2ext.h` | `9afc725e9dda7c8b476e7337e75b22fa660ed462c169e203ec28c10e62f91f4c` | MIT |
| `GLES2/gl2platform.h` | KhronosGroup/OpenGL-Registry `1cdd228e34966dd6b95bd203e9f84faba0f371a1` `api/GLES2/gl2platform.h` | `f5da0747540a50be5f44aad264aae45bdf157a192c40f17487dd9a2f99c71b6c` | Apache-2.0 |
| `GLES3/gl3.h` | KhronosGroup/OpenGL-Registry `1cdd228e34966dd6b95bd203e9f84faba0f371a1` `api/GLES3/gl3.h` | `a0e4880142bd059bd4d7446f257920b5020b8bbb0a86eb0149cbd1ea2fcf8cb0` | MIT |
| `GLES3/gl3platform.h` | KhronosGroup/OpenGL-Registry `1cdd228e34966dd6b95bd203e9f84faba0f371a1` `api/GLES3/gl3platform.h` | `a9e060dae5a2b11c5a889b679692b7089a10a7e03ebfbb6cf28217f6e322fb08` | Apache-2.0 |

`eglplatform.h` picks the Wayland native types (`struct wl_display *`,
`struct wl_egl_window *`) when `WL_EGL_PLATFORM` is defined, which
`<wayland-egl.h>` does, as upstream's does; otherwise the generic Unix types
(`void *` and `khronos_uintptr_t`).  `<wayland-egl.h>` and
`<wayland-egl-core.h>` are zedBSD's own (Zlib).

To update, fetch the same paths at a new registry commit, check that the
changes are declarations only, and replace this table.

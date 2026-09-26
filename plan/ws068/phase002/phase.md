<!-- awesome-plan project=zedbsd record=ws068p002 -->

# ws068-p002: EGL の核と libwayland-egl、display 直接の platform（clear だけの GLES で疎通）

Phase ID: `ws068-p002`
Parent: [WS068](../ws.md)
Status: cleared（q471-i01、2026-09-26）
Phase disposition: normal
Queue: q471-i01
承認: 2026-09-26 ユーザーの自律実行の指示（ws068-p001 と同じ）
設計: [design.md](../design.md) §2・§3

## 範囲

1. Khronos の header（EGL・GLES2・KHR）を `include/libc/` に、出典とライセンスの記録。
2. `libwayland-egl.so`（`wl_egl_window_*`）。
3. `libEGL.so`: display（Wayland・display 直接）、initialize、config、window surface（swapchain）、context、make current、
   swap buffers、swap interval、query、error、get proc address。
4. `libGLESv2.so` の最小: `glClearColor`・`glClear`・`glViewport`・`glGetString`・`glGetError`・`glFlush`・`glFinish`
   （clear を Vulkan の render pass の clear で）。GLES の方式に依らない部分だけ。
5. 試験 app `egltest`: 色を周期で変えて clear し、`eglSwapBuffers`。Wayland と display 直接。

## 受け入れ

1. Venus で、zwl --glass の窓に egltest の clear の色が出る（VNC の画面、`--expect` の色）。
2. display 直接（zwl 無し）で全画面に clear の色（画面）。
3. build は warning 0、新しい C は style-check 0。

## 結果（q471-i01、2026-09-26）

実装（新しい C は coding-style の全文、style-check 0）:

- header: Khronos の `EGL/egl.h`・`eglext.h`・`eglplatform.h`・`KHR/khrplatform.h`（EGL-Registry `db3425b8`）、`GLES2/gl2.h`・
  `gl2ext.h`・`gl2platform.h`・`GLES3/gl3.h`・`gl3platform.h`（OpenGL-Registry `1cdd228e`）を変えずに `include/libc/` へ、
  出典・SHA-256・ライセンス（Apache-2.0 と MIT）は `include/libc/EGL/API-PROVENANCE.md`。`wayland-egl.h`・`wayland-egl-core.h` は自前。
- `libwayland-egl.so`（`userland/base/libwayland-egl`）: `wl_egl_window_*`。中身（`wayland-egl-backend.h`）は libEGL と共有。
- `libEGL.so`（`userland/base/libegl`）: display（Wayland: `eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR)`・`eglGetDisplay(wl_display)`、
  display 直接: `EGL_DEFAULT_DISPLAY` → `VK_KHR_display` の最初の display・mode・plane、surfaceless）、initialize（VkInstance・VkDevice）、
  4 つの config（RGBA8／RGB8 × depth 無し／24+8。depth・stencil はまだ使われない）、window surface（swapchain、resize と
  OUT_OF_DATE で作り直し、swap interval 0 で MAILBOX）、context（ES 2・3）、make current（thread ごとの状態は pthread の key。
  `__thread` は共有 library で `__tls_get_addr` を要し `-z defs` で link できないため）、swap buffers（render pass の clear の色で
  frame を作り present、fence で待つ）、query、error、`eglGetProcAddress`（`dlopen(NULL)` の global scope から。loader に
  `RTLD_DEFAULT` が無い）。pbuffer・pixmap・EGLImage・fence sync は未。
- `libGLESv2.so`（`userland/base/libglesv2`）の最小: `glClearColor`・`glClear`・`glViewport`・`glGetIntegerv`（viewport）・`glGetString`・
  `glGetError`・`glFlush`・`glFinish`。
- `egltest`（`userland/base/egltest`）: EGL＋GLES の clear の試験 app（Wayland の窓と display 直接）。
- `platform/amd64/vmunix.mk` の link、zdesktop の 2 つの config に追加。

確認（Venus、QEMU）: `plan/ws068/tests/egl-p002.sh` PASS（`build/ws068-p002/run2/`・`run3/`）:
1. zwl --glass の窓に egltest の clear の色（3a78c8）、ドッキングで swapchain を作り直して全体が同じ色、バーの × で `EGLTEST DONE`。
2. zwl 無しの display 直接で全画面 1280x800 が c8503a、300 frame で `EGLTEST DONE`。
3. build は warning 0、style-check 0。回帰 p052・p069 PASS。

未実施: i915 実機（ws068-p006）。GLES の描画（p003、方式はユーザーの判断待ち）。

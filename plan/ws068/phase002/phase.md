<!-- awesome-plan project=zedbsd record=ws068p002 -->

# ws068-p002: EGL の核と libwayland-egl、display 直接の platform（clear だけの GLES で疎通）

Phase ID: `ws068-p002`
Parent: [WS068](../ws.md)
Status: in-progress（q471-i01）
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

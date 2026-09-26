<!-- awesome-plan project=zedbsd record=ws068-design -->

# WS068 設計: EGL と OpenGL ES を Vulkan と display 拡張の上に（ws068-p001、2026-09-26）

状態: 草案（エージェント作成）。**GLES の方式（§4）はユーザーの判断を待つ。** EGL（§2・§3）は方式によらず要るので、p002 は
この草案のまま進める（2026-09-26 のユーザーの自律実行の指示「デスクトップ関連の作業を優先しつつ…自走を続けて」による）。

## 1. 目的と範囲

- EGL 1.5 と OpenGL ES 2.0（次に 3.0）の API を、zedBSD の libvulkan（Vulkan 1.0 core と WSI）の上に実装する。GPU を叩くのは
  libvulkan だけ（Venus では host の Vulkan、i915 ではネイティブ実行器）。
- 窓: Wayland（zwl の Wiseman Mode とゲームモード）と、compositor の無い display 直接（`VK_KHR_display` の全画面）。
- 範囲外: desktop OpenGL（GLX が要るもの、§6）、`/dev/gpu0` を直接使う GL、EGLImage の外部 buffer（dma-buf）の import。

## 2. library の構成

| library | 中身 | 依存 |
| --- | --- | --- |
| `libEGL.so` | EGL 1.5 の API、display・config・context・surface、platform（Wayland・display 直接・surfaceless）、`eglGetProcAddress` | libvulkan、libwayland-client（Wayland の platform を使うときだけ、dlopen しない静的な依存でよい） |
| `libGLESv2.so` | GLES 2.0（3.0）の API と Vulkan への変換 | libvulkan、libEGL の内部 interface |
| `libwayland-egl.so` | `wl_egl_window_create`・`_resize`・`_destroy`・`_get_attached_size`（Wayland の標準の小さな library） | libwayland-client |

- EGL と GLES の間は非公開の interface（`struct zegl_context` の vtable と、現在の context の thread local）で結ぶ。GLES の
  各関数は現在の context を thread local から取る。
- header: Khronos の公開 header（`EGL/egl.h`・`EGL/eglext.h`・`EGL/eglplatform.h`・`GLES2/gl2.h`・`gl2ext.h`・
  `GLES3/gl3.h`・`KHR/khrplatform.h`）を `include/libc/` に置き、Vulkan と同じく出典（registry の commit と SHA-256）と
  ライセンス（EGL は Apache-2.0、GLES と khrplatform は MIT）を `API-PROVENANCE.md` に記録する。実装の code は取り込まない。
- `eglplatform.h` の native 型: `EGLNativeDisplayType` = `struct wl_display *`（または display 直接の `EGL_DEFAULT_DISPLAY`）、
  `EGLNativeWindowType` = `struct wl_egl_window *`（display 直接では `0` = 全画面）。

## 3. EGL の設計

- **display**: `eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl_display)` と `eglGetDisplay(wl_display)`（先頭の pointer を
  `wl_display` の interface と比べて見分ける）→ Wayland。`EGL_DEFAULT_DISPLAY` → 環境に `WAYLAND_DISPLAY` があれば Wayland、
  無ければ display 直接。`EGL_PLATFORM_SURFACELESS_MESA` → pbuffer だけ。
- **初期化**: `eglInitialize` で VkInstance（surface の拡張）と VkDevice（swapchain）を作り、display ごとに 1 つ持つ。
- **config**: RGBA8888（alpha 有無）× depth 0/24 × stencil 0/8 × samples 1 の組。`EGL_OPENGL_ES2_BIT`・`ES3_BIT`、
  `EGL_WINDOW_BIT`・`PBUFFER_BIT`。depth は `VK_FORMAT_D24_UNORM_S8_UINT` か `D32_SFLOAT`（device が持つもの）。
- **window surface**: Wayland は `wl_egl_window` の `wl_surface` から `VkSurfaceKHR`（`vkCreateWaylandSurfaceKHR`）、
  display 直接は最初の display・mode・plane で `vkCreateDisplayPlaneSurfaceKHR`。swapchain は FIFO、`eglSwapInterval(0)` で
  MAILBOX（あれば）。`wl_egl_window_resize` は次の `eglSwapBuffers` で swapchain を作り直す。
- **描画先**: GLES の default framebuffer は「acquire した swapchain の image」。1 frame の間の GL の命令は 1 つの command buffer に
  積み、`eglSwapBuffers` で submit と present。`glFinish`・`glReadPixels` はその場で submit して待つ。
- **pbuffer**: offscreen の VkImage。
- **context**: `eglCreateContext`（ES 2 / ES 3）、`eglMakeCurrent`（thread local）、共有 context（object の名前空間の共有）は後。
- **同期**: `EGL_KHR_fence_sync`（`VkFence`）は後。
- **error**: EGL の error は thread local の `eglGetError`。

## 4. GLES の方式（ユーザーの判断を待つ）

| 方式 | 中身 | 利点 | 欠点 | 規模 |
| --- | --- | --- | --- | --- |
| **A. 自前の変換層＋自前の GLSL ES compiler（C）** | GL の状態 → Vulkan の pipeline の cache、GLSL ES 1.00（次に 3.00）→ SPIR-V を C で自作 | 外部 code 無し、ライセンスが単純、SPIR-V を i915 のネイティブ compiler の受ける形（OpSwitch 無し等）で出せる | compiler の作業が大きい（GLSL ES 1.00 の parser・型検査・SPIR-V 出力） | 中〜大 |
| **B. 自前の変換層＋glslang（C++、BSD-3/Apache-2.0）** | 変換層は A と同じ。GLSL → SPIR-V は glslang を `userland/packages/` で取得・patch（libc++ は既にある） | compiler の正しさを借りられる、GLSL ES 3.00 も同じ | C++ の外部 package、glslang の SPIR-V は i915 の compiler の知らない命令（OpSwitch 等）を含みうる（F-023） | 中 |
| C. Mesa の Zink（MIT） | Mesa の gallium・NIR・GLSL compiler ごと移植、Zink が Vulkan へ | 完成度（desktop GL と GLX も同じ道） | Mesa の build（meson・python）、Vulkan 1.1 以上と多くの拡張を要求（libvulkan・i915 実行器の拡張が大きい） | 大 |
| D. ANGLE | GLES＋EGL を Vulkan の上に | Chromium と共有 | C++17・GN、Vulkan 1.1 相当、build 環境が重い | 特大 |

エージェントの推奨: **B**（変換層は自前の C、compiler は glslang）で GLES 2.0 を先に通し、i915 で glslang の出力が通らない所は
変換層で SPIR-V を整える（または F-023 の実行器の補い）。desktop GL と GLX（§6）が要るなら、そのとき C（Zink）を再検討する。

判断の要点: 外部の C++ の package を許すか（B・C・D）、GLX（desktop GL）をいつ要るか（C を早めるか）。

## 5. 試験

- `egltest`（userland/base/egltest）: EGL＋GLES 2.0 の小さな試験 app。p002 は clear だけ（色を周期で変える）、p003 から
  三角形・texture・blend・depth・resize。Wayland（zwl --glass の窓）と display 直接（zwl の無い guest）の両方。
- 判定は画面（Venus の VNC、i915 の capture）の読み取り。

## 6. GLX（2026-09-26 ユーザー追加「WaylandコンポジタのX11サーバ機能については、GLX拡張も実装しておいてください」）

- zwl の X11 対応（Xwayland に当たるもの、新 WS）の GLX 拡張は desktop OpenGL の context を要る（GLX 1.4、indirect は不可、
  direct rendering の client 側 `libGL.so` が要る）。
- 方式: (1) GLES の変換層の上に desktop GL の compatibility の部分集合を足す、(2) Zink（§4 C）で desktop GL と GLX を得る。
  (1) は GLES の方式が A・B のとき、(2) は C のとき。X11 の WS の設計で決める。

## 7. libvulkan への要求（洗い出し）

- 今ある: Vulkan 1.0 core、`VK_KHR_surface`・`VK_KHR_wayland_surface`・`VK_KHR_display`・`VK_KHR_swapchain`・
  `VK_KHR_display_swapchain`。
- GLES 2.0 の変換層（A・B）に要る: 1.0 core で足りる（dynamic state の viewport・scissor、push constant か UBO、
  combined image sampler）。`VK_KHR_maintenance1`（負の viewport の高さで GL の上下を合わせる）はあると楽だが、shader で
  y を反転すれば無くてよい。
- i915 実機: F-023（OpSwitch・triangle strip・vkFreeDescriptorSets・`gl_VertexIndex` 等）が GLES のアプリで効く。
  GL_TRIANGLE_STRIP・GL_TRIANGLE_FAN は変換層で triangle list に展開できる。

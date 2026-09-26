<!-- awesome-plan project=zedbsd record=ws068 -->

# WS068: EGL と OpenGL ES を Vulkan と display 拡張の上に実装する

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: q476
Resume point: p010（pbuffer）。次に p009（frame を重ねて速く）、p011（FBO）。p003（GLSL の compiler）は GLES の方式（design.md §4）のユーザーの判断を待つ
<!-- awesome-plan-current:end -->

## 目標

2026-09-26 ユーザー: 「では、Vulkanとディスプレイ拡張の上に、EGLとGLESを実装するWSを作っておいてください。」（直前の質問: EGL+OpenGL ES のコードをこの Wayland で動かすときの、libGLESv2/v3 の移植（`/dev/gpu0` を直接使う方式と libvulkan で変換する方式）と、Wayland とディスプレイ直接の両方で使える EGL の実装の費用）

EGL と OpenGL ES（2.0、次に 3.0）を、zedBSD の libvulkan（Vulkan）と `VK_KHR_display`（display 拡張）の上に実装する。EGL + GLES で書かれた Linux 等のアプリの source を、Wayland（zwl・Wiseman Mode）の窓と、compositor の無い display 直接（全画面）の両方で動かす。GPU を叩くのは libvulkan だけにし、GL のための別の GPU 経路（`/dev/gpu0` の直接の GL 実装、virgl の GL 経路、i915 の GL driver）は作らない。

## 方式の前提（2026-09-26 の見積もりの回答から。p001 で確定）

| 部分 | 方式 | 見積もり（目安） |
| --- | --- | --- |
| EGL | 自前。`EGLSurface` → `VkSurfaceKHR`（Wayland: `VK_KHR_wayland_surface`、display 直接: `VK_KHR_display`）＋ swapchain、`eglSwapBuffers` → present。config・context・pbuffer。`libwayland-egl`（`wl_egl_window`） | 小（Phase 2〜3 個）。EGLImage は後 |
| GLES 2.0 | 自前の変換層（GL の状態 → Vulkan の pipeline の cache、GLSL ES → SPIR-V は glslang（BSD）の移植）、または Mesa の Zink の移植（MIT）。p001 で比べる | 中 |
| GLES 3.0 | 2.0 の延長（UBO、instancing、MRT、transform feedback、format） | 大 |
| ANGLE | Chromium（WS035 の最後）が使う。GLES＋EGL を Vulkan の上に持つが、C++・GN の build・Vulkan 1.1 相当の要求。Chromium の着手の時に再検討 | 大〜特大 |

採らない方式: `/dev/gpu0` の上の GL の直接実装（Venus の QEMU では virgl の GL の経路と Mesa の virgl driver、i915 では DRM の無い上に iris 相当か第 2 の実行器が要り、backend ごとに二重の保守になる）。

## 依存と制約

- libvulkan の機能: 今は Vulkan 1.0 core と WSI。GLES の変換層・Zink・ANGLE の要る拡張（maintenance1、dedicated allocation 等）と Vulkan 1.1 を足す必要があるかを p001 で洗う。
- i915 の実機: ネイティブ実行器の不足（[F-022](../future-work.md): context の間の画像の共有、[F-023](../future-work.md): builtin・OpSwitch・triangle strip・vkFreeDescriptorSets・fence 等）が GLES の shader と Wayland の窓で効く。Venus（QEMU）では libvulkan が host へ転送するので先に通せる。
- 外部の source（glslang、Mesa 等）は `userland/packages/` に tarball で取得・検証・patch し、ライセンスを監査する（AGENTS.md）。
- EGL・GLES の header は Khronos の公開 header（Apache-2.0 / MIT 系、監査する）。

## 受け入れ（p001 で確定する）

1. Wayland（zwl の Wiseman Mode とゲームモード）と display 直接の両方で、EGL＋GLES 2.0 の試験アプリ（三角形、texture、blend、depth、窓の resize）が描け、画面の読み取りで一致する（Venus）。
2. GLES 3.0 の代表機能の試験。
3. i915 実機（F-022・F-023 の後）で同じ試験。実機の証拠と QEMU の証拠を分ける。
4. 規約・回帰。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws068-p001](phase001/phase.md) | 設計: library の構成（`libEGL.so`・`libGLESv2.so`・`libwayland-egl.so`）、GLES の方式の比較（自前の変換層＋glslang / Zink / ANGLE）、libvulkan に要る機能、試験アプリ、ライセンス（[design.md](design.md)） | cleared（q470-i01。GLES の方式はユーザーの判断待ち） | — |
| [ws068-p002](phase002/phase.md) | EGL の核と `libwayland-egl`、display 直接の platform（最初は clear だけの GLES で疎通） | cleared（q471-i01、2026-09-26。Venus で Wayland と display 直接の clear） | p001 |
| ws068-p003 | GLSL ES の source を SPIR-V に（compiler。変換層は p008 にある） | planning | p008、GLES の方式のユーザーの判断（design.md §4） |
| ws068-p004 | GLES 2.0 の残りと試験の充実 | planning | p003 |
| ws068-p005 | GLES 3.0 | planning | p004 |
| ws068-p006 | i915 実機での確認 | planning | p004、F-022、F-023 |
| ws068-p007 | 規約の全文との照合と回帰（最後） | planning | 全 Phase |
| [ws068-p008](phase008/phase.md) | GLES 2.0 の描画の核（SPIR-V の shader binary、変換層。compiler の方式に依らない部分） | cleared（q475-i01。Venus で strip・texture・blend・depth・cull、display 直接と窓と resize） | p002 |
| ws068-p009 | frame を 2〜3 枚重ねる（EGL の frame in flight。Venus で clear だけ 125 ms/frame、WSI 直接は 50 ms） | planned | p008 |
| [ws068-p010](phase010/phase.md) | EGL の pbuffer（offscreen。GLX の描画先） | in-progress（q476-i01） | p008 |
| ws068-p011 | framebuffer object・renderbuffer・cube map（texture への描画） | planned | p008 |

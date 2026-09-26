<!-- awesome-plan project=zedbsd record=ws068p010 -->

# ws068-p010: EGL の pbuffer（offscreen の描画先）

Phase ID: `ws068-p010`
Parent: [WS068](../ws.md)
Status: cleared（q476-i01、2026-09-26）
Phase disposition: normal
Queue: q476-i01
承認: 2026-09-26 ユーザーの自律実行の指示（EGL/GLES と X11 の GLX を含むデスクトップ関連を優先）
設計: [design.md](../design.md) §3（pbuffer は offscreen の VkImage）

## 背景

GLX（ws069-p004）の最初の形は、libGL が offscreen に描いて glReadPixels の画素を X の窓へ PutImage する（Mesa の xlib の道と同じ）。
その offscreen が EGL の pbuffer。p008 の frame と readback はそのまま使える。

## 範囲

1. `eglCreatePbufferSurface`（EGL_WIDTH・EGL_HEIGHT、既定 0 は 1）: R8G8B8A8 の color image と、config にあれば depth buffer。
   config は `EGL_PBUFFER_BIT` も持つ（surfaceless の display では pbuffer だけ）。
2. frame: acquire・present が無い。image は常に 1 枚。pass の前後の layout は COLOR_ATTACHMENT_OPTIMAL（present の layout は
   swapchain の image だけ）。`eglSwapBuffers` は描いたものを submit して待つ（EGL では pbuffer の swap は効果が無いが、GLES の
   frame の資源を解放する区切りにする）。
3. glReadPixels の後（submit して待った後）も GLES の frame の資源を解放する（readback を毎 frame する使い方で stream が
   増え続けないように）。
4. egltest `--platform=pbuffer`: 画面に出さず scene を描いて glReadPixels で確かめる。

## 受け入れ

1. Venus で `egltest --platform=pbuffer --scene=draw` の glReadPixels の 11 点が一致し、何百 frame 回しても終わる（stream が増えない）。
2. p008・p002 の試験が変わらず通る。build warning 0、新しい C の style-check 0。
3. i915 実機は範囲外（ws068-p006）。

## 結果（2026-09-26、q476-i01）

cleared。受け入れ 1〜3 を満たした。

### 実装

- libEGL: `eglCreatePbufferSurface`（EGL_WIDTH・EGL_HEIGHT、0 は 1、16384 まで）。config はすべて `EGL_WINDOW_BIT | EGL_PBUFFER_BIT`
  （surfaceless の display は pbuffer だけ）。`EGL_SWAP_BEHAVIOR` は pbuffer では `EGL_BUFFER_PRESERVED`。
- vulkan.c: `zegl_pbuffer_open`（R8G8B8A8 の color image、depth buffer、pass、framebuffer）。surface ごとの `rest_layout`（window は
  PRESENT_SRC、pbuffer は COLOR_ATTACHMENT_OPTIMAL）で pass の前後の layout を決める。pbuffer の最初の frame で image を clear して
  rest の layout へ。pbuffer の frame の最初の pass は、全体の clear でなければ load（中身を frame をまたいで保つ）。
  `eglSwapBuffers` は submit して待ち、GLES の frame の資源を解放する。depth image に TRANSFER_DST（clear のため）。
- libGLESv2: draw・clear・readback は window と pbuffer の両方へ。glReadPixels の後（submit して待った後）に frame の資源を解放する。
- egltest: `--platform=pbuffer`（EGL_DEFAULT_DISPLAY の上の pbuffer、`--size` の大きさ）。

### 検証（QEMU・Venus。i915 実機は未実施）

- `plan/ws068/tests/egl-p010.sh` PASS: 320x200 の pbuffer に 600 frame、glReadPixels の 11 点が一致、DONE。2048x1536（画面より
  大きい）でも一致。
- 回帰: `egl-p008.sh` PASS（build/ws068-p010-p008.log）、`egl-p002.sh` PASS。build warning 0、style-check 0。
- boot test PASS（build/ws068-p010-boot/login.png）。

### 分かったこと

- pbuffer（present も acquire も無い）でも 1 frame が約 95 ms（600 frame で 57 s）。submit と fence の待ちの往復が Venus で遅い
  （F-021 に追記）。EGL で frame を重ねる（p009）と隠せる。

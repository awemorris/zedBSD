<!-- awesome-plan project=zedbsd record=ws068p010 -->

# ws068-p010: EGL の pbuffer（offscreen の描画先）

Phase ID: `ws068-p010`
Parent: [WS068](../ws.md)
Status: in-progress（q476-i01）
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

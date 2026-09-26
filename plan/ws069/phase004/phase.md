<!-- awesome-plan project=zedbsd record=ws069p004 -->

# ws069-p004: GLX の核（Xzed の GLX 拡張、libGL の GLX、pbuffer に描いて X の窓へ）

Phase ID: `ws069-p004`
Parent: [WS069](../ws.md)
Status: in-progress（q477-i01）
Phase disposition: normal
Queue: q477-i01
承認: 2026-09-26 ユーザー「WaylandコンポジタのX11サーバ機能については、GLX拡張も実装しておいてください。」と自律実行の指示
設計: [design.md](../design.md) §4。GLES の変換層（ws068-p008）と pbuffer（ws068-p010）の上に作る。GLSL の compiler（ws068-p003）は
使わない（方式はユーザーの判断待ち）ので、この Phase の GL は GLES 2.0 の API と SPIR-V の shader。固定機能の GL 1.x は p005。

## 方式

- direct rendering（Mesa の xlib・swrast の道と同じ形）: libGL が EGL の pbuffer に描き、`glXSwapBuffers` で glReadPixels した画素を
  Xzed の private request（XzedPutImageRGB24、複数行を 1 request で）で X の窓へ写す。Xzed は rootless なら窓の Wayland の surface
  へ、rootful なら screen へ合成する（既存の道）。画像の共有（DRI3 に当たる）は後の Phase。
- Xzed の GLX 拡張: `QueryExtension("GLX")` に major opcode 144 で答え、GLX の QueryVersion（1.4）・QueryServerString・
  QueryExtensionsString・ClientInfo に答える。描画の request（indirect rendering）は持たない（BadRequest）。
- libGL.so: libGLESv2 の sources（GL の入口と変換層）＋ `userland/X11/libGL/glx.c`（GLX）＋ libX11（xlib.c）の内部の写し。export は
  gl* と glX* だけ。EGL の display は surfaceless（`EGL_PLATFORM_SURFACELESS_MESA`、pbuffer だけ）。
- header: `GL/glx.h`・`GL/gl.h`（自前、Zlib。この Phase は型・GLX の定数・GLES 2.0 の関数の宣言まで。GL 1.x は p005）。

## 範囲

1. Xzed: QueryExtension と GLX の問い合わせ（`userland/X11/xzed/glx.c`）。
2. libX11: `XQueryExtension`、GLX の request を送る private の `XzedExtensionRequest`、`XzedPutImageRGB24` を複数行の request に
   （最後に 1 回だけ同期）。
3. libGL: `glXQueryExtension`・`glXQueryVersion`・`glXGetClientString`・`glXQueryServerString`・`glXQueryExtensionsString`・
   `glXChooseVisual`・`glXGetConfig`・`glXCreateContext`・`glXDestroyContext`・`glXMakeCurrent`・`glXSwapBuffers`・`glXIsDirect`・
   `glXGetCurrentContext`・`glXGetCurrentDrawable`・`glXGetCurrentDisplay`・`glXWaitGL`・`glXWaitX`・`glXGetProcAddress(ARB)`、
   GLX 1.3 の `glXGetFBConfigs`・`glXChooseFBConfig`・`glXGetFBConfigAttrib`・`glXGetVisualFromFBConfig`・`glXCreateNewContext`・
   `glXMakeContextCurrent`・`glXCreateWindow`・`glXDestroyWindow`。窓の大きさが変われば pbuffer を作り直す。
4. glxtest（`userland/X11/glxtest`）: GLX の文字列を出し、egltest の scene（SPIR-V）を X の窓に描き、glReadPixels で確かめる。

## 受け入れ

1. Venus の zwl で Xzed（rootless）の上の glxtest が、GLX の拡張と版（1.4）を server から得て、scene を X の窓（Wiseman の窓）に
   描く（画面の読み取りと glReadPixels の両方で期待の色）。
2. 既存の X の試験（x11-p003）と EGL の試験（p008・p010）が通る。build warning 0、新しい C の style-check 0、手を入れた legacy の
   file は HEAD より悪くしない。
3. i915 実機は範囲外（ws069-p006）。

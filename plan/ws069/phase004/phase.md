<!-- awesome-plan project=zedbsd record=ws069p004 -->

# ws069-p004: GLX の核（Xzed の GLX 拡張、libGL の GLX、pbuffer に描いて X の窓へ）

Phase ID: `ws069-p004`
Parent: [WS069](../ws.md)
Status: cleared（q477-i01、2026-09-26）
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

## 結果（2026-09-26、q477-i01）

cleared。受け入れ 1〜3 を満たした。

### 実装

- Xzed（`userland/X11/xzed/glx.c`・`glx.h`、main.c の dispatch）: QueryExtension（98）に GLX を major opcode 144 で答える
  （他の名前は無し）。GLX の QueryVersion（1.4）・QueryExtensionsString・QueryServerString（vendor「zedBSD」、version「1.4」、
  extensions）・ClientInfo に答え、それ以外（indirect rendering）は BadRequest。
- libX11（xlib.c）: `XQueryExtension`、private の `XzedExtensionRequest`（extension の request と reply の本体）。
  `XzedPutImageRGB24` は 1 request に入るだけの行を送る（以前は 1 行ごと）。Xzed は client の未処理の入力を 1 MiB までしか
  持たないので、request ごとに同期する（大きな窓で SIGPIPE になったのを直した）。
- header: `GL/gl.h`（GLES2/gl2.h の上。GL 1.x は p005）、`GL/glx.h`（GLX 1.4 の定数・型・関数）、`X11/Xutil.h`（XVisualInfo）、
  Xlib.h に Visual、X.h に VisualID、Xzed.h に `XzedExtensionRequest`。
- libGL.so（`userland/X11/libGL`）: libGLESv2 の sources と `glx.c` と libX11 の写しを 1 つに。export は gl* と glX* だけ。
  GLX の context は surfaceless の EGL display の上の ES 2 の context と窓の大きさの pbuffer。`glXSwapBuffers` は glReadPixels
  → RGB の行（上から）→ `XzedPutImageRGB24` → `eglSwapBuffers`（frame の資源の解放）→ 窓の大きさが変われば pbuffer を
  作り直す。visual は 0x21（depth 無し）と 0x22（depth 24・stencil 8）、GLX 1.3 の config も同じ 2 つ。
- glxtest（`userland/X11/glxtest`）: GLX の版と文字列を出し、egltest の scene を X の窓に描き、最初の frame を読み返す。
- image: config-amd64-zdesktop.mk に libgl・glxtest。

### 検証（QEMU・Venus。i915 実機は未実施）

- `plan/ws069/tests/x11-p004.sh` PASS（build/ws069-p004.log）: zwl＋Xzed --rootless の上で glxtest が GLX 1.4 と server の文字列を
  得て（`direct=1`）、X の窓（Wiseman の窓「GLX test」）の画面 10 点と glReadPixels の 11 点が期待の色。docked（1280x762）でも
  画面 10 点が一致（pbuffer の作り直し）。× で glxtest が終わり Xzed は残る。画面: build/ws069-p004/{glx,docked}.png。
- 回帰: x11-p002（rootful の zterm、画面を目視: build/ws069-p002/typed.png で入力と出力が見える）、x11-p003 PASS、egl-p008 PASS、
  egl-p010 PASS。build warning 0。新しい C の style-check 0、xlib.c は 188 → 186、Xzed の main.c は 437 → 437。
- boot test PASS（build/ws069-p004-boot/login.png）。

### 分かったこと・制限

- rootless の X の screen は `--size`（既定 1280x800）の大きさで、それより大きい X の窓は切れる（F-024）。
- GL は GLES 2.0 の API と SPIR-V の shader だけ。desktop の GL 1.x の固定機能は p005、GLSL は ws068-p003（方式の判断待ち）。
- 1 frame は pbuffer への描画・readback・PutImage で Venus では約 100 ms 台（F-021）。画像の共有（DRI3 に当たる）は後。

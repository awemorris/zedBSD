<!-- awesome-plan project=zedbsd record=ws069-design -->

# WS069 設計: zwl で X11 の app を動かす（ws069-p001、2026-09-26）

## 1. 構成

- X server は既存の Xzed（`userland/X11/xzed`、X11 core protocol の小さな実装、窓ごとの画素を保持して CPU で合成）。
  これに Wayland backend を足し、`Xzed --wayland` で zwl の client として動く。`/dev/graphics` と `/dev/input` は使わない。
- X の client は今まで通り `DISPLAY=:0`（`/tmp/.X11-unix/X0`）。App Home からの起動には `DISPLAY` を渡す。
- 新しい file: `xzed/wayland.c`（Wayland の窓・buffer・seat）、`xzed/glyphs.c`（libtruetype の等幅 font の glyph）。
  `main.c` は backend の選択と present・text の分岐だけ変える。

## 2. rootful（p002）

- 1 つの xdg_toplevel（題名「X11」）、大きさは `--size`（既定 1280x800）。X の root window がその大きさ。
- present: Xzed の合成済みの screen（`s->screen`、XRGB）を wl_shm の buffer（2 枚、release で交互）へ dirty 矩形だけ写し、
  attach・damage・commit。
- 入力: wl_pointer の enter・motion・button・axis → Xzed の pointer frame（absolute、窓の座標 = X の screen の座標）、
  wl_keyboard の key（evdev）→ Xzed の keycode（input.c の対応を関数として出す）、modifiers。
- text: `KERN_GRAPHICS_GET_GLYPH` の代わりに libtruetype で 8x16 の cell に描いた 1 bit の glyph（`zed-unicode` の metrics のまま）。
- zwl の configure の大きさの変化: p002 は無視（X の screen の大きさは固定）。

## 3. rootless（p003）

- root window は見せない。root の子（top-level）の窓が map されると、その窓の xdg_toplevel と wl_shm の buffer を作る。
  題名は WM_NAME。top-level の画素（子を合成）を buffer へ。
- 入力は focus のある Wayland の窓の top-level へ、座標は top-level の原点から。
- zwl からの configure（大きさ）→ X の ConfigureNotify、close → WM_DELETE_WINDOW か client の切断。
- 位置は Wiseman が決める（X の窓の x・y は 0 とみなす）。override-redirect（menu 等）は後。

## 4. GLX（p004）

- Xzed に GLX 拡張（QueryExtension「GLX」、glXQueryVersion・QueryServerString・CreateContext 等の最小）。
- client の `libGL.so`（GLX と GL の入口）: context は EGL（WS068）の surfaceless か pbuffer で作り、GLES（WS068 の変換層）で
  描く。`glXSwapBuffers` で描いた画像を Xzed へ渡す: 画像の共有（`GPU_CAP_SHARE` の fd、libvulkan の外部 memory）を
  Unix socket の SCM_RIGHTS で送る Xzed の拡張（DRI3 の PixmapFromBuffer と Present に当たる）。Xzed は rootless の
  窓の Wayland の surface にその buffer を渡す（zwl の GPU の buffer の import）か、CPU で読んで窓の画素へ写す（最初の段階）。
- desktop GL（GL 1.x〜3.x の固定機能・compatibility）は GLES の上の部分集合として段階的に（WS068 design.md §6）。
  GLX の app の多くは desktop GL を要るので、GLES の方式の判断（WS068 §4）と合わせて範囲を決める。

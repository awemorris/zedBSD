<!-- awesome-plan project=zedbsd record=ws035p074 -->

# ws035-p074: 非公開の header（`Xzed.h`、`zed_gpu_buffer_v1`）と libzdesktop の役割

Phase ID: `ws035-p074`
Parent: [WS035](../ws.md)
Status: in-progress（q487-i01）
Phase disposition: normal
Queue: q487-i01
承認: 2026-09-27 ユーザー「Xzed.hはパブリックヘッダにする必要がないかも？zed-gpu-buffer-v1-client-protocol.h もパブリックに
しなくていいよね。」「libzdesktopは、zdesktop Waylandコンポジタの非標準のxdgをラップするライブラリです。」、libzdesktop の役割は
「両方」（非標準の拡張の wrapper と OS・daemon への道）、「続けてください。」

## 調べたこと

- `zed_gpu_buffer_v1` を話すのは libvulkan の Wayland の WSI（`wsi-wayland.c`）と libwayland だけ。app（mview・wltest・
  zdesktop-terminal・libEGL）は標準の Vulkan の WSI を使い、この protocol を直接使わない。
- `X11/Xzed.h` を使うのは libX11・libGL（GLX）・zterm・zshell・zwm（木の中の X の app）。

## 範囲

1. `include/libc/wayland/zed-gpu-buffer-v1-client-protocol.h` → `userland/base/libwayland/`（libwayland と libvulkan の WSI の
   非公開の header）。公開の `wayland-client-protocol.h`・`xdg-shell-client-protocol.h` の `struct zed_gpu_buffer_v1;` を外す。
   libwayland の export（`zed_gpu_buffer_v1_*`）は libvulkan・libzdesktop のため残す。
2. `include/libc/X11/Xzed.h` → `userland/X11/libX11/Xzed.h`（木の中の X11 の code の非公開の header）。
3. `include/libc/zdesktop.h` の説明: libzdesktop は zdesktop の非標準の Wayland/xdg 拡張の wrapper と OS・daemon への道の両方。
   GPU の buffer の共有の API は最初の使い手（zdesktop-x11server の GLX の画像の受け渡し）と一緒に足す（それまでは約束しない）。

## 受け入れ

1. build（amd64 の zdesktop の image と、X11 の app を含む別の platform の userland の config 1 つ）が warning 0。
   sysroot の公開 header の元（`include/libc`）に 2 つの header が無く、木の中に `<X11/Xzed.h>`・`<wayland/zed-gpu-buffer-v1-client-protocol.h>` の
   include が無い。
2. 意味を変えない変更なので、回帰は build と Venus の egl-p008（Wayland の WSI）・x11-p005（Xzed.h の使い手）と最後の boot test。

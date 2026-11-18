---
id: apps.videoplayer.fullscreen
title: Video Player を F11・Alt+Enter・double click で全画面にし、Esc で戻す
status: active
areas: [videoplayer, compositor]
paths: [userland/desktop/videoplayer/main.c, userland/desktop/wayland/protocol.c, userland/desktop/wayland/scanout.c, userland/desktop/wayland/content-type.c, userland/desktop/libkeiland-backend-zedbsd/scanout-zedbsd.c]
machine: either
human: look
since: ws122-p005a
---

## 目的
動画の player が F11・Alt+Enter・picture の double click で全画面になり、Esc・F11 で戻ることを確かめる（BUG-223、ws122-p005a）。

## 準備
user で `videoplayer /tmp/aat-samples/sample.mp4` を開く。

## 操作と確認
1. 操作: picture を click してから F11。
   確認事項: 全画面。正解: `VIDEOPLAYER FULLSCREEN on=1`（F11 は compositor の menu の Full Screen の項が取ることもある）、`ZWL CONFIGURE … width=1280 height=800 fullscreen=1` の後に `VIDEOPLAYER PRESENTED width=1280 height=800`（新しい大きさの最初の frame。撮影はその後）。確認方法: log、撮影（全画面、2 秒後に bar が消える）。
   game mode（ws122-p005b）: player は content type video を付ける（`ZWL CONTENT surface=N type=video`）。pointer が 2 秒止まると `ZWL SCANOUT direct=1`（直の scanout）、QEMU の display が共有の image を出せない時は `ZWL SCANOUT direct=0 reason=backend`。撮影の前に `direct=0 reason=shot`（撮影は合成の frame）。
2. 操作: Esc。
   確認事項: 戻る。正解: `VIDEOPLAYER FULLSCREEN on=0`、`ZWL CONFIGURE … fullscreen=0` の大きさが出力の大きさでない（元の window の大きさ）。確認方法: log。
3. 操作: Alt+Enter、もう一度 F11。
   確認事項: 入って出る。正解: `VIDEOPLAYER FULLSCREEN toggle via=alt-enter` と `on=1`、F11 で compositor の `ZWL GLASS fullscreen-leave … via=f11`（BUG-194）か `VIDEOPLAYER FULLSCREEN on=0`、戻りの `ZWL CONFIGURE … fullscreen=0` は出力の大きさでない。確認方法: log。
4. 操作: picture を double click、もう一度 double click。
   確認事項: 入って出る。正解: `VIDEOPLAYER FULLSCREEN toggle via=double-click` が 2 回、`on=1` と `on=0`。確認方法: log。

## 合格
各段の log の行と、撮影で全画面の picture。

## 注記
T1-235: f11.png が左上の 960x600 だった。撮影は `on=1` の 0.8 秒後で、player の新しい swapchain の最初の frame の前だったと推定する（`VIDEOPLAYER PRESENTED` で確かめる）。また Esc のすぐ後の Alt+Enter は、player がまだ描いていなかった全画面の image の大きさを戻り先として覚え、F11 で 1280x800 の window に戻った（protocol.c の `zwl_window_enter_fullscreen` を直した）。

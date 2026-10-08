---
id: apps.browser.video
title: Browser で page の <video>（controls）を再生し、click で一時停止する
status: active
areas: [browser, media, audio]
paths: [userland/desktop/libbrowser/page/media.c, userland/desktop/libbrowser/bind/media.c, userland/desktop/libmedia/]
machine: either
human: look
since: ws121
---

## 目的
Browser が page の `<video>` を libmedia（libavcodec の add-in）で再生し、controls の帯の click で再生・一時停止することを確かめる（WS121 p004〜p006）。ベータ3 までは browser の音は無い（WS191 の H3 (c) の決定で libmedia から音を外した）。

## 準備
runner の試料 `/tmp/aat-samples/sample.mp4`（12 秒、MPEG-4 Part 2 640x360 と AAC）。`/tmp/aat-work/video.html` に `<video src="/tmp/aat-samples/sample.mp4" controls>`（muted でない）。image に libavcodec。

## 操作と確認
1. 操作: kei として `/bin/browser /tmp/aat-work/video.html`。
   確認事項: 開く。正解: `BROWSER MEDIA MEDIA open width=640 height=360 … video=mpeg4 audio=none`（ベータ3 で音が戻れば `audio=aac` も正解）、page の左上に 640x360 の最初の絵と下の帯（再生の印）。確認方法: log、撮影。
2. 操作: 動画の上を click、3 秒待つ。
   確認事項: 再生。正解: `BROWSER MEDIA MEDIA play position_ms=`、絵が進み、帯は一時停止の印と進んだ位置。確認方法: log、撮影。
3. 操作: もう一度 click。
   確認事項: 一時停止。正解: `BROWSER MEDIA MEDIA pause position_ms=` が 2000 以上。確認方法: log。

## 合格
1〜3 の正解。音はベータ3 まで無い（`audio=none` を許す）。音が戻った後も QEMU では耳で聞かない（実機で）。

## 注記
助け: `plan/tools/aat/scenarios/helpers_browser_media.py`。page に style が無いので video は page の (8, 8) に、page は窓の本体の左上から。

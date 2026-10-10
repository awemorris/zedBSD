---
id: apps.videoplayer.play
title: Video Player で MP4 を再生・一時停止・送り・終わりまで
status: active
areas: [videoplayer, libmedia, libavcodec, audiod]
paths: [userland/desktop/videoplayer/, userland/desktop/mediafile/, userland/desktop/libmedia/, userland/desktop/media-app/, userland/packages/multimedia/libavcodec/]
machine: either
human: look
since: ws122
---

## 目的
Video Player が app の任意 libavcodec による MPEG-4 Part 2 の映像と、libmedia 自前 AAC-LC の音の MP4を開いて再生し、操作が効くことを確かめる。

## 準備
host で作った 12 秒・640x360・音つきの `sample.mp4` を target の `/tmp/aat-samples/` に置く（`plan/tools/aat/scenarios/samples.py`）。kei で `videoplayer /tmp/aat-samples/sample.mp4` を開く。

## 操作と確認
1. 操作: 開くのを待つ。
   確認事項: 開き。正解: `VIDEOPLAYER OPEN path=/tmp/aat-samples/sample.mp4 width=640 height=360 duration_ms=約12000 video=mpeg4/libavcodec audio=aac-lc/libmedia`、`VIDEOPLAYER PLAY`。確認方法: log。
2. 操作: 1 秒後に撮る。
   確認事項: 再生。正解: `VIDEOPLAYER FRAMES shown=N` の N が増える、絵が動く。確認方法: log、撮影 2 枚（人が見る）。
3. 操作: Space、Space。
   確認事項: 一時停止と再開。正解: `VIDEOPLAYER PAUSE shown=…`、`VIDEOPLAYER PLAY shown=…`。確認方法: log。
4. 操作: 右矢印。
   確認事項: 送り。正解: `VIDEOPLAYER SEEK to_ms=…`。確認方法: log。
5. 操作: 終わりまで待つ（最大 10 秒）。
   確認事項: 終わり。正解: `VIDEOPLAYER ENDED shown=N`。確認方法: log。

## 合格
1〜5 の log。見えは needs-person。音の有無は UAT。

## 注記
sample は 12 秒（T1-232: 4 秒では window・撮影の 2 回の間に終わり、一時停止が効く前に END になった）。

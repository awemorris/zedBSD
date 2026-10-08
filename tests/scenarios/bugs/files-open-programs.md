---
id: bugs.files-open-programs
title: Files: x の bit の付いた動画は Video Player で、command line の program は残る Terminal で開く
status: active
areas: [files]
paths: [userland/desktop/files/apps.c, userland/desktop/files/ui-desktop.c]
machine: either
human: look
since: BUG-233
---

## 目的
BUG-233（動画の file を開くと Terminal が開くように見える）と BUG-234（/bin の file を開くと何も起きないように見える）を desktop の icon の double click で確かめる。

## 準備
AAT の image と試験の file（`/tmp/aat-samples/sample.mp4`）。helper が `~/Desktop` に `aat-clip.mp4`（mode 755）と `aat-ls`（/bin/ls の写し）を置き、終わりに消す。

## 操作と確認
1. 操作: desktop の `aat-clip.mp4` を double click。
   確認事項: Video Player で開く（BUG-233）。正解: `ZFILES DESKTOP open name=aat-clip.mp4 via=double-click`、`ZFILES LAUNCH name=Video Player command=/bin/videoplayer …aat-clip.mp4`、`/bin/videoplayer` が動いている（desktop の Files が立てた Video Player の行は session の log に来ない、T1-488）、`ZTERM START` が無い。確認方法: log、撮影 video。
2. 操作: desktop の `aat-ls` を double click。
   確認事項: Terminal で走り、終わっても窓が残る（BUG-234）。正解: `ZTERM START`、その窓の `KWL CLIENT gone` が 3 秒の間に無い。確認方法: log、撮影 ls（出力と終了の案内）。

## 合格
1・2 の行。違えば BUG-233・BUG-234 の再現（fail）。撮影は人が見る（needs-person）。

## 注記
実機の FAT の stick の動画は UAT。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。

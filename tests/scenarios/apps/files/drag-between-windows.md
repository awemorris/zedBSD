---
id: apps.files.drag-between-windows
title: Files の項目を別の Files の窓と Terminal へ drag できる（DnD の回帰）
status: active
areas: [files, terminal, compositor, dnd]
paths: [userland/desktop/files/dnd.c, userland/desktop/files/ui-drag.c, userland/desktop/files/main.c, userland/desktop/terminal/window.c, userland/desktop/terminal/clipboard.c, userland/desktop/libkeiland/ui/clipboard.c, userland/desktop/wayland/data.c]
machine: either
human: none
since: ws189-p002
---

## 目的
ws189 の DnD の変更（compositor の multi-device・receive の制限・release の待ち・drag の icon、libkeiland の clipboard.c）の後も、前からある Files の
file の drag（別の窓の folder への移動、Terminal への path）が動くことを確かめる回帰。

## 準備
desktop。kei で `mkdir -p ~/dnd-src ~/dnd-dst && echo one > ~/dnd-src/a.txt && echo two > ~/dnd-src/b.txt`（`~/dnd-dst/a.txt` が無いこと）。Files の窓を 2 つ開き
（1 つ目は `files ~/dnd-src`、2 つ目は `files ~/dnd-dst`）、重ならないように置く。Terminal を 1 つ開く。

## 操作と確認
1. 操作: 1 つ目の窓の `a.txt` を押して 2 つ目の窓の一覧の空いた所へ動かし、止めて離す。`aat mark start` は押す前。
   確認事項: 窓の間の drag。正解: `ZFILES DND start items=1`、`KWL DATA drag start`、`KWL DATA drag enter`（2 つ目の窓の client）、`KWL DATA drag drop`、
   2 つ目の窓の `ZFILES DROP operation=… items=1 destination=/home/kei/dnd-dst first=/home/kei/dnd-src/a.txt` と `ZFILES TASK done … state=done`、1 つ目の窓の
   `ZFILES DND end dropped=1`。`ls ~/dnd-dst` に a.txt（operation が move なら `~/dnd-src` から消える。どちらだったかを記録）。確認方法: log、kei の `ls`。
2. 操作: 1 つ目の窓の `b.txt` を Terminal の窓の上へ drag して離す。
   確認事項: Terminal への path。正解: `ZTERM DROP enter uris=1`、`ZTERM DROP bytes=… uris=1`、Terminal の入力行に `/home/kei/dnd-src/b.txt`。確認方法: log、撮影。
3. 操作: 1 つ目の窓の `b.txt` を押して 20 px 動かし、40 ms で離す。これを 3 回。
   確認事項: 即離し（T1-436 F6 の回帰）。正解: `KWL CLIENT gone` が無く、Files の窓が残って次の click に答える。確認方法: log、撮影。

## 合格
1〜3 の正解。

## 注記
後片付け: `rm -r ~/dnd-src ~/dnd-dst`（guest の中）。

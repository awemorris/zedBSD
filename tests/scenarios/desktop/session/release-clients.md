---
id: desktop.session.release-clients
title: 20 個の client を一度に終わらせた時、compositor がすぐ手放す
status: active
areas: [compositor]
paths: [userland/desktop/wayland/objects.c, userland/desktop/wayland/main.c]
machine: either
human: none
since: BUG-239
---

## 目的
死んだ client の後始末が遅い（QEMU で 20 個に 23 秒、その間 key も遅れる、BUG-239）かを測り、直した後に確かめる。

## 準備
desktop。

## 操作と確認
1. 操作: Terminal を 20 個起動し、全部の `ZTERM START` を待つ。
   確認事項: 20 個が開く。正解: `ZTERM START` が 20 行。確認方法: log。
2. 操作: `ps` で `/bin/terminal` の pid を集めて `kill`（root。zedBSD に pkill は無い）。
   確認事項: compositor が全部を手放す時間と、その内訳。正解: `KWL CLEANUP done client=… ms=… quiesce=… surfaces=… shell=… objects=… queued=…` が 20 行、全部が 10 秒以内。gone の client の buffer（import）は後で 1 つずつ解放され、`KWL RETIRE drained released=… ms=…` が 120 秒以内に出る（BUG-239）。確認方法: log（各行と合計、`KWL RETIRE slow` の行を記録に残す）。解放は compositor が暇な時（frame が飛んでいない、input が 200 ms 無い、描く物が無い。1 秒待たされた list は進む）だけ行うので、kill の後の `KWL PERF compose … frame_ms=` は各窓で 100 ms 以下（T1-392 では 510〜604 ms）。

## 合格
20 行が 10 秒以内（目標）、`KWL RETIRE drained` が 120 秒以内、kill の後の PERF の frame_ms の最大が 100 ms 以下。内訳は記録に残す（どこで時間を使うかの測定を兼ねる）。

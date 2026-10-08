---
id: desktop.dnd.text-between-windows
title: Text Editor の選択を別の窓へ drag すると、そこに文字が入り、印が「コピー可」になる
status: draft
areas: [compositor, dnd, textedit, terminal]
paths: [userland/desktop/wayland/data.c, userland/desktop/wayland/dnd-state.c, userland/desktop/libkeiland/ui/clipboard.c, userland/desktop/textedit/]
machine: either
human: look
since: ws189-p003
---

## 目的
app の間の drag and drop（WS189）: 文字の drag が compositor を通って別の窓に入ること、受ける窓の上で compositor の印が「コピー可」（緑の +）になること、
drag の元の窓へ戻すと印が出ず、離しても何も起きないこと（Text Editor は自分への drop を受けない、ws189 の決定）。

## 準備
desktop。Text Editor を 2 つ（別の process、kei で `textedit` を 2 回）と Terminal を 1 つ開き、重ならないように置く。1 つ目の Text Editor に `hello drag` と打つ。

## 操作と確認
1. 操作: 1 つ目の Text Editor で `hello drag` を全部選ぶ（Ctrl+A）。`aat mark start`。
   確認事項: 選択。正解: 選択が見える。確認方法: 撮影。
2. 操作: 選択の中で左を押し（`aat down`）、20 px 動かす。
   確認事項: drag の始まり。正解: `TEXTEDIT DND drag start bytes=10`、`KWL DATA drag start` の行（types=2 actions=1）、`KWL DATA drag state=neutral`（元の窓の上）。確認方法: `aat wait-log 'KWL DATA drag start' --since start`。
3. 操作: pointer を 2 つ目の Text Editor の本文の中ほどへ動かし（数段で）、止めて撮る。
   確認事項: 印と挿入点。正解: `KWL DATA drag state=copy`。撮影で pointer の右下に緑の丸と白い +、2 つ目の窓の本文に点滅しない accent の縦線（挿入点）。確認方法: log、撮影（人が見る）。
4. 操作: 離す（`aat up`）。
   確認事項: drop。正解: `KWL DATA drag drop`、`KWL DATA receive ... source=`（2 つ目の窓の client）、2 つ目の窓に `hello drag`（`TEXTEDIT DND drop bytes=10`）。確認方法: log、撮影。
5. 操作: 同じく選んで drag し、Terminal の上で離す。
   確認事項: Terminal。正解: `ZTERM DROP enter ... text=1`、Terminal の行に `hello drag`。確認方法: log、撮影。
6. 操作: 同じく選んで drag し、1 つ目の Text Editor（元）の本文の上に戻して止め、離す。
   確認事項: 自分の窓。正解: そこでの `KWL DATA drag state=neutral`（copy の行が先に出ない）、離すと `KWL DATA drag cancel`、本文は変わらない。確認方法: log、撮影。

## 合格
2〜6 の log がそろい、受けた窓と Terminal に文字が入り、元の窓は変わらない。撮影の印と挿入点は人が見る（needs-person の look）。

## 注記
drop の前の受け渡しの拒否（データは落とした窓にだけ）は desktop.dnd.early-receive で確かめる。

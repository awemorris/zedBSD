---
id: desktop.dnd.early-receive
title: drag の data は落とした窓にだけ渡り、drop の前の受け取りは空になる
status: draft
areas: [compositor, dnd]
paths: [userland/desktop/wayland/data.c, userland/tests/data-probe/]
machine: either
human: none
since: ws189-p002
---

## 目的
ws189 の原則「データは落とした窓にだけ渡す」（Wayland からの意図した逸脱、設計 §1）: drop の前に receive した client は空を読み、drop の後は中身を読むこと。

## 準備
desktop。試験の image は `plan/ws189/tests/config-amd64-aat-dnd.mk`（AAT の image に `data-probe`）。kei で `data-probe --token=dnd` を開き、Terminal（`echo secrettext` を打って Enter）と重ならないように置く。

## 操作と確認
1. 操作: Terminal の出力の `secrettext` を double-click で選び、その上で押して data-probe の窓の上へ動かし、止める。`aat mark start` は押す前。
   確認事項: drop の前の受け取り。正解: `DATAPROBE drag enter text=1`、`DATAPROBE drag received when=early bytes=0`、compositor の `KWL DATA receive ... refused=not-dropped`。確認方法: log。
2. 操作: 離す。
   確認事項: drop の後。正解: `DATAPROBE drag drop`、`DATAPROBE drag received when=drop bytes=10 text=secrettext`、`KWL DATA drag finish`。確認方法: log。

## 合格
early が bytes=0 で refused の行があり、drop の後に中身が届く。

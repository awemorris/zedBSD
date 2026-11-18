---
id: desktop.dnd.refused-mark
title: 受けない窓の上の drag は「不可」の印になり、離すと取り消される
status: active
areas: [compositor, dnd]
paths: [userland/desktop/wayland/data.c, userland/desktop/wayland/dnd-state.c, userland/desktop/wayland/shell.c]
machine: either
human: look
since: ws189-p002
---

## 目的
compositor の受け入れの印（ws189 の決定「コピー可・不可」）: 文字を受けない窓の上で赤い進入禁止の印が出て、離すと drop にならないこと。

## 準備
desktop。Terminal（`echo dndtest` を打って Enter）と Photos を開き、重ならないように置く。

## 操作と確認
1. 操作: Terminal の出力の `dndtest` を double-click で選び、その上で押して Photos の窓の中ほどへ動かして止め、撮る。`aat mark start` は押す前。
   確認事項: 印。正解: `KWL DATA drag enter`（Photos の client）、`KWL DATA drag accept ... mime=(none)`、`KWL DATA drag state=refused`。撮影で pointer の右下に赤い丸と白い横棒。確認方法: log、撮影（人が見る）。
2. 操作: 離す。
   確認事項: 取り消し。正解: `KWL DATA drag cancel ... reason=release`、`KWL DATA drag drop` が無い、`ZTERM DRAG done dropped=0`。確認方法: log。
3. 操作: 同じく drag し、bar・窓の外（壁紙だけの所でなく、desktop の icon も無い窓の題の上）で止める。
   確認事項: target の無い所。正解: `state=refused`。確認方法: log。

## 合格
1 と 2 の log、撮影の赤い印（人が見る）。

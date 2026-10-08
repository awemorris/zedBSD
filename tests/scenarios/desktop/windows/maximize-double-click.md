---
id: desktop.windows.maximize-double-click
title: title bar の double click で最大化し（離した時に決める）、0.2 秒以内に描き直す。2 回目で押したまま動かすと移動
status: active
areas: [compositor, windows]
paths: [userland/desktop/wayland/shell.c, userland/desktop/wayland/title-tap.c, userland/desktop/wayland/toplevel.c]
machine: either
human: look
since: BUG-179
---

## 目的
double click で最大化し、中身が最大の大きさで描き直されるまでの時間を測る（BUG-179、目標 0.1〜0.2 秒）。double click は 2 回目を離した時に決まり、2 回目で押したまま動かす（touch pad・touch screen の tap and drag）と最大化せず窓が動く（BUG-265）。

## 準備
App Home から Files を開く（浮いた窓）。

## 操作と確認
1. 操作: title bar の空いた所を double click。
   確認事項: 最大化。正解: `KWL GLASS double-click wait surface=S`（2 回目の press）の後、離した時に `KWL GLASS dock surface=S via=double-click …`。確認方法: log。
2. 操作: 描き直しを待つ。
   確認事項: 時間。正解: `KWL GLASS resized surface=S docked=1 … after_ms=N` の N ≤ 200。確認方法: log の after_ms、撮影。
3. 操作: 最大化を戻し（restore の button）、浮いた title bar を click し、すぐ 2 回目を押したまま 80 px 右下へ動かして離す（`aat down`・`aat rel 80 80`・`aat up`）。
   確認事項: tap and drag（BUG-265）。正解: `KWL GLASS double-click wait surface=S`、`KWL GLASS double-click moved surface=S`、`KWL GLASS dock` の行が増えず、窓が 80 px 動いた所に浮いたまま。確認方法: log、撮影。

## 合格
1 で最大化し、2 の N ≤ 200（N が大きい時は値を書いて needs-person、QEMU は遅い）、3 で最大化せず動く。

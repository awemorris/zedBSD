---
id: apps.notes.fill-region
title: Notes の塗りつぶしで閉じた円の中と PDF の囲みの中を塗り、消しゴムと undo で戻す
status: active
areas: [notes, libpdf]
paths: [userland/desktop/notes/, userland/desktop/libpdf/]
machine: either
human: look
since: ws216-p007
---

## 目的
Notes の塗りつぶしの道具が、閉じた線で囲まれた所を線の下に塗り、同じ所の 2 度目と drag を断り、消しゴムで塗りだけを消して undo で戻せ、PDF の頁の絵の囲みも塗れることを確かめる（WS216、ws216-p005・p006）。

## 準備
- 補助（`plan/tools/aat/scenarios/helpers_notes_edit.py`）が host で `plan/tools/aat/make-edit-samples.py` の `edit-basic.pdf`（3 頁、US Letter。1 頁に JPEG（320x200 を (72, 420) に 240x150 pt、色の階調の中に白い正方形））を作り、target の scenario の folder（`/tmp/aat-work/notes-fill-region-<時刻>/`）に `notes-edit.pdf` として置く。
- user として Notes を同じ folder の新しい notebook `fill.pdf` で開く（`NOTES START`）。座標は Notes の `NOTES LAYOUT` と窓の位置から決める。

## 操作と確認
1. 操作: key P（ペン）、mouse で頁の真ん中に閉じた円（端は始めの点を越える）。
   確認事項: 円。正解: `NOTES TOOL 1 name=pen`、`NOTES STROKE page=0 … tool=0`。確認方法: log。
2. 操作: key F（塗り）、円の中心を click。撮影 `filled`。
   確認事項: 塗り。正解: `NOTES TOOL 52 name=fill`、`NOTES FILL bands=B background_us=0`（背景の無い頁）、`NOTES FILL made place=0 …`（線の下）。撮影で円の内側が塗られ、線は上に見え、線と塗りの間に白い隙間が無い。確認方法: log、撮影（人が見る）。
3. 操作: 同じ所をもう一度 click。撮影 `already-filled`。
   確認事項: 同じ色の 2 度目。正解: `NOTES FILL refused reason=same`、撮影に notice「Already filled」。確認方法: log、撮影（人が見る）。
4. 操作: 塗りの道具のまま円の中で drag（約 60 px）。撮影 `tap-to-fill`。
   確認事項: drag は塗らない。正解: `NOTES FILL moved`、撮影に status「Tap to fill」。確認方法: log、撮影（人が見る）。
5. 操作: key E（消しゴム、線ごと。`parts=1` ならもう一度 E）、円の中心を click。撮影 `erased`。Ctrl+Z。撮影 `undone`。
   確認事項: 消しゴムは見えている塗りだけを消す（D7 (c)）。正解: `NOTES ERASE page=0 removed=1 strokes=1`（円は残る）、`NOTES UNDO page=0 strokes=2`。撮影で塗りが消え、undo で戻る。確認方法: log、撮影（人が見る）。
6. 操作: key P で Pen に戻し、Notes を閉じる（Ctrl+W）。同じ folder の `notes-edit.pdf` を開き、key F、1 頁の画像の白い正方形の中心（PDF の (192, 495)）を click。撮影 `pdf-filled`。
   確認事項: PDF の頁の囲みの塗り。正解: `NOTES FILL bands=B background_us=U`（U > 0: 頁の絵を帯ごとに描いた）、`NOTES FILL made place=0 …`。撮影で白い正方形が塗られ、周りの階調は塗られない。確認方法: log、撮影（人が見る）。
7. 操作: key P、Ctrl+S、閉じる。host へ file を取り、`qpdf --check`、`pdftoppm -r 72` で 1 頁を描く（`…-poppler.png`）。
   確認事項: 保存した PDF。正解: `NOTES SAVE reason=request pages=3 strokes=1`、`qpdf --check` に誤りが無い、Poppler の頁でも正方形が塗られている。確認方法: log、host の command、画像（人が見る）。

## 合格
1〜7 の正解。判定は log と撮影（PNG）で、撮影の見た目は人（または T1）が見る（needs-person）。

## 注記
- 補助は `apps.notes.fill-region`（`helpers_notes_edit.py`）。塗りの時間は `NOTES FILL made … ms=` と `background_us` の行が記録に残る（QEMU の amd64 の数。Pi 4 の数は ws216-p009）。
- 塗りは window を止める（上限なし、ws216 の決定）。補助は `NOTES FILL made` を 120 秒まで待つ。
- 指での塗り（Finger on、8 px の閾値）は実機 5330 でユーザーが見る（ws216-p007 の別の項目）。

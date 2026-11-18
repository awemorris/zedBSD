---
id: apps.notes.pdf-insert-image
title: Notes で PDF に PNG と JPEG の画像を挿入する
status: active
areas: [notes, libpdf]
paths: [userland/desktop/notes/, userland/desktop/libpdf/, userland/base/libz-compat/]
machine: either
human: none
since: ws175-p001
---

## 目的
Image の道具で PNG（alpha つき）と JPEG を PDF の頁に挿入でき、保存・開き直しの後も編集できることを確かめる（WS175、ws175-p006・p007・p008）。

## 準備
- 補助（`helpers_notes_edit.py`）が `edit-basic.pdf` を `notes-edit.pdf` として、`picture.png`（alpha つき）・`landscape.jpg` と一緒に scenario の folder（`/tmp/aat-work/notes-<id>-<時刻>/`）に置き、Notes で開く。
- 2 頁を表示（Page Down を 1 回）。

## 操作と確認
1. 操作: toolbar の Select の道具を押し、出てきた Image を押して、file chooser（題「Insert Image」）で `picture.png`（160x100、右半分が半透明）を選ぶ（名前の頭の p を打って Enter）。
   確認事項: 挿入。正解: 見えている頁の中央に画像が選ばれた状態で現れ（頁の幅・高さの半分以内、72 dpi の大きさ以内）、透けた部分から下の頁が見える。確認方法: log `NOTES EDIT insert page=1 kind=image object=I image=160x100`、撮影。
2. 操作: 挿入した画像を左上へ drag。
   確認事項: 移動。正解: 画像が動く。確認方法: log `NOTES EDIT move page=1 object=I dx= dy=`。
3. 操作: もう一度 toolbar の Image で `landscape.jpg`（200x120）を選ぶ（l と Enter）。
   確認事項: 2 つ目の挿入。正解: 中央に JPEG。確認方法: log `NOTES EDIT insert page=1 kind=image object=J`（J ≠ I）。
4. 操作: Ctrl+S、Notes を閉じ、同じ file を Notes で開き直して 2 頁へ。
   確認事項: 開き直し。正解: 2 つの画像が同じ場所に見え、Select の道具の click で選べる。確認方法: log `NOTES OPEN … kind=annotated path=…` の次の `NOTES EDITS opened edits=N edited_pages=1 rebased=0`、`NOTES EDIT select page=1 … kind=image inserted=1`、撮影。
5. 操作: host へ file を取り、`qpdf --check`、`pdfimages -list`（無ければ `pdftoppm`）。
   確認事項: 保存した PDF。正解: qpdf がエラーを出さない。2 頁に画像が 2 つ増え、PNG の画像に SMask が有る。確認方法: command の出力。

## 合格
1〜5 の正解。

## 注記
ws175-p010 で active（補助 `helpers_notes_edit.py`）。file の大きさ（deflate の有無、D4）は記録するが合否に使わない。

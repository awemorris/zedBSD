---
id: apps.notes.pdf-edit-multipage
title: Notes で複数頁の PDF を頁ごとに編集し、pen の線と一緒に保存して開き直す
status: active
areas: [notes, libpdf]
paths: [userland/desktop/notes/, userland/desktop/libpdf/]
machine: either
human: none
since: ws175-p001
---

## 目的
複数頁の PDF の、別々の頁（回転した頁を含む）の編集が頁ごとに保たれ、pen の書き込みも残り、保存して開き直した後も編集が残り、Reset で元の姿に戻せることを確かめる（WS175、ws175-p007・p008）。

## 準備
- 補助（`helpers_notes_edit.py`）が `edit-basic.pdf`（3 頁。3 頁は `/Rotate 90` で 2 行の文字）を scenario の folder に `notes-edit.pdf` として置き、Notes で開く。1 頁を表示。

## 操作と確認
1. 操作: Select の道具で 1 頁の画像を右へ drag。
   確認事項: 1 頁の編集。正解: 画像が動く。確認方法: log `NOTES EDIT move page=0`。
2. 操作: PageDown を 2 回で 3 頁へ。3 頁の文字の行を click して Delete。
   確認事項: 回転した頁の編集。正解: `NOTES PAGE current=2 count=3`、行が消える。確認方法: log `NOTES EDIT delete page=2`、撮影。
3. 操作: pen の道具で 3 頁に線を 1 本描く。
   確認事項: pen。正解: `NOTES STROKE page=2`。確認方法: log。
4. 操作: Ctrl+S、Notes を閉じ、同じ file を開き直す。
   確認事項: 開き直し。正解: `NOTES OPEN pages=3 strokes=1 kind=annotated path=…` と `NOTES EDITS opened edits=2 edited_pages=2 rebased=0`。1 頁の画像は右に、3 頁の行は無く線が有る。確認方法: log、撮影（1 頁と 3 頁）。
5. 操作: 1 頁へ戻り、Select の道具で右へ動かした画像を click し、toolbar の Reset。
   確認事項: 開き直した後の取り消し。正解: 画像が元の位置に戻る（undo の履歴は file に残らないので、開き直した後の取り消しは Reset。design.md §6.2）。確認方法: log `NOTES EDIT reset page=0`、撮影。
6. 操作: host へ file を取り、`qpdf --check`、`pdftoppm` で 2 頁を元の file の 2 頁と比べる。
   確認事項: 編集していない頁。正解: 2 頁の画素が元と同じ。確認方法: 画像の比較。

## 合格
1〜6 の正解。

## 注記
ws175-p010 で active（補助 `helpers_notes_edit.py`。6 は 2 頁を `pdftoppm -r 50` で元と画素で比べる）。Notes は undo の履歴を file に保存しない（開き直すと履歴は空）。開き直した後は Reset で元の姿に戻す（design.md §6.2、review の L10）。消した物は選べないので、開き直した後には戻せない（design.md §9）。

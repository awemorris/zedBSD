---
id: apps.notes.text-box-follow
title: Notes の文字の box が窓の大きさに付いて動き、box を開いたまま保存する
status: active
areas: [notes]
paths: [userland/desktop/notes/]
machine: either
human: look
since: ws177-p012
---

## 目的
文字の編集の box を開いたまま窓の大きさが変わると、box が編集している行に付いて動くこと（前は開いた時の窓の位置に留まった）、box を開いたまま Ctrl+S で保存すると、打った文字が保たれて保存されることを確かめる（ws177-p012、backlog-p2 の 7・10）。

## 準備
- 補助（`helpers_notes_edit.py`）が `edit-basic.pdf` を scenario の folder に `notes-edit.pdf` として置き、Notes で開く（apps.notes.pdf-edit-text と同じ）。
- toolbar の Select の道具（`NOTES TOOL 16 name=select`）。

## 操作と確認
1. 操作: 段落の 1 行目を double-click。
   確認事項: 行の下の 1 行の box。正解: `NOTES TEXT box open kind=line page=0 object=I font=original rect=X,Y,W,H`。確認方法: log、撮影。
2. 操作: F11（全画面）。
   確認事項: box が行に付いて動く。正解: box が開いたまま（`NOTES TEXT box close` が無い）、`NOTES TEXT box moved rect=…` が出て、撮影で box が 1 行目の下にある。確認方法: log、撮影。
3. 操作: End の後に ` again` と打ち、Ctrl+Z、Ctrl+Shift+Z、Ctrl+S。
   確認事項: box の中の undo・redo（打鍵ごと、ws177-p013）と、box を開いたままの保存。正解: Ctrl+Z で `NOTES TEXT box reported=… bytes=48`（49 から 1 字戻る）、Ctrl+Shift+Z で `bytes=49`、Ctrl+S で `NOTES EDIT text page=0 object=I kind=line … font=original`、`NOTES TEXT box close`、`NOTES SAVE reason=request`。確認方法: log。
4. 操作: F11 で戻し、Ctrl+W。
   確認事項: 閉じる。正解: `NOTES EXIT`。保存した file の `pdftotext -f 1 -l 1` に「again」。確認方法: log、host の pdftotext。

## 合格
1〜4 の正解。撮影の box の位置は人が見る。

## 注記
指の zoom・scroll（box が付いて動く、box の上の指の tap で caret、外の tap で確定）は AAT に touch の口が無いので 5330 の touch の UAT で見る。System Menu の無い compositor での Ctrl+S・Ctrl+W（menu が key を取らない時に box を通る）も同じく UAT。

---
id: apps.notes.pdf-edit-text
title: Notes で PDF の中の文字を書き換え、消す
status: active
areas: [notes, libpdf]
paths: [userland/desktop/notes/, userland/desktop/libpdf/]
machine: either
human: none
since: ws175-p001
---

## 目的
既存の文字の行を、元の埋め込みの font のまま書き換えられること、元の font に無い文字では置き換えの font になり利用者に知らされること、行を消せることを確かめる（WS175、ws175-p002・p004・p005・p008）。

## 準備
- 補助（`helpers_notes_edit.py`）が `edit-basic.pdf`（1 頁の段落の 1 行目は「The quick brown fox jumps over the lazy dog」。DejaVu Sans を段落の字だけに subset した TrueType（`ABCDEF+DejaVuSans`、大文字は T だけ））を scenario の folder に `notes-edit.pdf` として置き、Notes で開く。
- toolbar の Select の道具（`NOTES TOOL 16 name=select`）。

## 操作と確認
1. 操作: 段落の 1 行目を click し、続けて double-click。
   確認事項: 選択と編集の box。正解: 1 回目の click で行に青い枠と handle、toolbar に Edit・Font（Original）・A-・A+・Delete・Reset。double-click で行の下に 1 行の編集の box（行の文字と末尾の caret）。確認方法: log `NOTES EDIT select page=0 object=I kind=text inserted=0 clipped=0 fixed=0 text="The quick brown fox`、`NOTES TEXT box open kind=line page=0 object=I font=original rect=X,Y,W,H`、撮影。
2. 操作: End、Backspace を 3 回（`dog` を消す）、`fox` と打つ。0.2 秒待って撮影し、Enter。
   確認事項: 打つ間の頁の表示と確定（元の font のまま）。正解: 打っている間に頁の行が「… the lazy fox」に変わり（0.1 秒ごとの preview）、Enter で box が閉じ、行の見た目の font が他の行と同じ。確認方法: log `NOTES EDIT text page=0 object=I kind=line chars=43 font=original fallback=0`、`NOTES TEXT box close`、撮影。
3. 操作: 2 行目を double-click（選んでいなければ click してから）し、End の後に ` ZEBRA` と打って Esc。
   確認事項: 置き換えの font。正解: 行の文字が変わり、status に「The line's font lacks some of these characters; it now uses Sans」（subset に Z・E・B・R・A の大文字が無いため）。確認方法: log `NOTES EDIT text page=0 object=J kind=line … font=original fallback=1`、撮影。
4. 操作: 3 行目を click して Delete キー。
   確認事項: 行の削除。正解: 3 行目が消え、4 行目以降は動かない。確認方法: log `NOTES EDIT delete page=0 object=K`、撮影。
5. 操作: 4 行目を click し、toolbar の Font を 1 回押す。
   確認事項: font の変更。正解: 4 行目が Sans（Mahora）で描かれ、toolbar の Font の名前が Sans になる。確認方法: log `NOTES EDIT font page=0 object=L font=sans fallback=1`、撮影。
6. 操作: Ctrl+S。host へ file を取り `pdftotext -f 1 -l 1` と `pdffonts`。
   確認事項: 保存した文字。正解: 「the lazy fox」と「ZEBRA」が出て、元の 3 行目の文字は出ない。pdffonts に置き換えの font の subset が `emb yes sub yes uni yes` で載る。確認方法: command の出力。

## 合格
1〜6 の正解。

## 注記
ws175-p010 で active（補助 `helpers_notes_edit.py`）。pdftotext は最新の revision の page を読む（消した行の bytes は base の revision に残る。D1）。
編集の box は libkeiland の field（`kl_field`、US 配列の文字と IME の文字）で、行の下に出る。どの font にも無い文字は editor が受けないので box が開いたまま status に理由が出る（host 試験 `host-notes-edit` の `notes_page_try_edit` の項目で確かめている）。

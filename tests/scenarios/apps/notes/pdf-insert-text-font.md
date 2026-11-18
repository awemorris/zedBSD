---
id: apps.notes.pdf-insert-text-font
title: Notes で PDF に文字を挿入し、font を変える（日本語を含む）
status: active
areas: [notes, libpdf, ime]
paths: [userland/desktop/notes/, userland/desktop/libpdf/]
machine: either
human: none
since: ws175-p001
---

## 目的
Text の道具で新しい文字を挿入し、font を Sans・Mono・Japanese に変えられること、IME で日本語を打てること、保存した PDF の文字が他の reader で読めること（埋め込みの subset と ToUnicode）を確かめる（WS175、ws175-p005・p008）。

## 準備
- 補助（`helpers_notes_edit.py`）が `edit-basic.pdf` を scenario の folder に `notes-edit.pdf` として置き、Notes で開く。1 頁の下の余白（画像の下）が見えている。
- 入力方式を日本語にしてある（補助は Settings の Languages で method 1 を選び、終わりに元へ戻す。`apps.terminal.japanese-history` と同じ）。

## 操作と確認
1. 操作: toolbar の Text の道具を押し、1 頁の下の余白を click して `Hello Notes` と打つ。
   確認事項: 挿入の box。正解: toolbar に Font（Sans）・A-・12 pt・A+ と 5 色。click した所に複数行の box が出て、打った文字が box に出る。確認方法: log `NOTES TOOL 41 name=text`、`NOTES TEXT box open kind=new page=0 object=-1 font=sans rect=X,Y,W,H`、撮影。
2. 操作: Esc で確定。
   確認事項: 挿入。正解: box が閉じ、文字が click した所を左上として頁に描かれる（12 pt、黒）。確認方法: log `NOTES EDIT text page=0 object=I kind=new chars=11 font=sans size=12 color=1f1f1fff`、撮影。
3. 操作: Select の道具を押し、挿入した文字を click して toolbar の Font を 1 回押す。
   確認事項: font の変更。正解: 文字が等幅の font に変わり、Font の名前が Mono になる。確認方法: log `NOTES EDIT select page=0 object=I kind=text inserted=1`、`NOTES EDIT font page=0 object=I font=mono`、撮影。
4. 操作: Text の道具に戻し、toolbar の Font を Japanese まで押し（Sans→Mono→Japanese）、別の余白を click、入力方式を日本語に切り替えて `nihongo` と打ち変換して確定、Esc。
   確認事項: 日本語。正解: 変換の間は box の caret の所に下線つきの preedit、候補の窓は box の caret の近く。確定で「日本語」が box に入り、Esc で頁に描かれる。確認方法: log `NOTES EDIT text page=0 object=J kind=new chars=3 font=cjk size=12 color=…`、撮影。
5. 操作: Ctrl+S。host へ file を取り `pdftotext -f 1 -l 1` と `pdffonts`。
   確認事項: 保存した PDF。正解: 「Hello Notes」と「日本語」が出る。pdffonts に Mono の font と DroidSansFallback の subset が `emb yes sub yes uni yes` で載る。確認方法: command の出力。

## 合格
1〜5 の正解。

## 注記
ws175-p010 で active（補助 `helpers_notes_edit.py`。ws079-p017 の Notes の IME の text box も兼ねる）。日本語は Alt+Space で ja にして `nihongo`・Space・Enter（`apps.terminal.japanese-history` と同じ手順）、Alt+Space で戻して Esc。挿入の文字の log は fallback を出さない（挿入は常に頼んだ font、無い字は fallback の font が描く）。
box は libkeiland の text area（`kl_text_area`、Enter は改行）で、Esc か box の外の click で確定する。drag して開くと、drag の幅で折り返す。

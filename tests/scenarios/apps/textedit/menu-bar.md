---
id: apps.textedit.menu-bar
title: Text Editor の menu bar（下線の付いた共通の menu）と Find の panel
status: active
areas: [textedit, compositor, menu]
paths: [userland/desktop/textedit/menu.c, userland/desktop/textedit/main.c, userland/desktop/wayland/menu-shell.c]
machine: either
human: none
since: BUG-248
---

## 目的
Text Editor の title bar に、他の app（Notes・Terminal など）と同じ下線の付いた menu bar（File・Edit・View・Help）が出て、項目と短縮 key が今までどおり効くことを確かめる（BUG-248）。title bar の Open・Save・Undo・Redo の button と Find の欄は無くなり、Find は窓の中の panel になる。

## 準備
user で `textedit /tmp/aat-work/menu.txt`（新しい file）を開く。

## 操作と確認
1. 操作: 窓が出るのを待つ。
   確認事項: menu bar。正解: title bar に File・Edit・View・Help（下線付き）。`KWL MENU bar client=C surface=S where=floating item=1`〜`item=4` の行があり、`TEXTEDIT TITLEBAR ready` の行が無い。確認方法: log、撮影。
2. 操作: F10、次に Esc。
   確認事項: File の menu。正解: File が開いて閉じる（`KWL MENU open client=C surface=S item=1`・`KWL MENU close`）。確認方法: log、撮影。
3. 操作: 本文を click し `one two one` と打ち、Ctrl+F、`one` と打ち、Enter、Esc。
   確認事項: Find の panel。正解: `TEXTEDIT FIND open find=`、打つと最初の `one` が選ばれ、Enter で次の `one`、Esc で `TEXTEDIT FIND closed`。確認方法: log、撮影（panel が開いている時）。
4. 操作: Ctrl+H、次に Esc。
   確認事項: Replace の panel。正解: `TEXTEDIT REPLACE open find=one`・`TEXTEDIT REPLACE closed`。確認方法: log。
5. 操作: Ctrl+S。
   確認事項: 短縮 key。正解: `TEXTEDIT SAVE path=/tmp/aat-work/menu.txt bytes=…`、file が `one two one`。確認方法: log、file を読む。

## 合格
1〜5 の正解。

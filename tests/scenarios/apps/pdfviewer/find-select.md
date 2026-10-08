---
id: apps.pdfviewer.find-select
title: PDF Viewer で文字を検索し、選択して copy する
status: active
areas: [pdfviewer, libpdf]
paths: [userland/desktop/pdfviewer/, userland/base/libpdf/]
machine: either
human: look
since: ws128-p004
---

## 目的
PDF Viewer の Find（titlebar の find field、F3・Shift+F3）と、文字の drag の選択・Ctrl+C の copy を確かめる（ws128-p004）。

## 準備
補助（`plan/tools/aat/scenarios/helpers_pdfviewer_find.py`）が host で `plan/ws175/tests/make-edit-samples.py` の `edit-basic.pdf`（3 頁。1 頁の段落の 1 行目は「The quick brown fox jumps over the lazy dog」、3 頁は `/Rotate 90`）を作り、target の `/tmp/aat-work/pdf-find.pdf` に置く。

## 操作と確認
1. 操作: kei で `pdfviewer /tmp/aat-work/pdf-find.pdf`。
   確認事項: 開き。正解: `PDFVIEWER READY … pages=3`。確認方法: log、撮影。
2. 操作: Ctrl+F、`lazy` と打つ。
   確認事項: 検索。正解: titlebar の find field に文字が入り、1 頁の「lazy」が橙で塗られる。確認方法: log `PDFVIEWER FIND found query="lazy" page=0 from=35 length=4`、撮影（人が見る）。
3. 操作: field を空にして `line` と打ち、Enter を 2 回。
   確認事項: 次の一致。正解: 1 頁の 3 行目・4 行目の「line」、次に 3 頁（回転した頁）の「line」へ移る。確認方法: log `PDFVIEWER FIND found query="line" page=2`、撮影。
3a. 操作: そのまま `s` を打つ（ws177-p043）。
   確認事項: Enter の後の field。正解: caret が末尾にあり選択が無いので、`s` は words に足される（「lines」。置き換わって「s」にならない）。確認方法: log `PDFVIEWER FIND none query="lines"`（または found）、compositor の `KWL TITLEBAR focus … edit=1`。その後 Backspace で「line」に戻す。
4. 操作: Esc、Home、1 頁の 1 行目の「The」の T から「quick」の k まで drag し、Ctrl+C。
   確認事項: 選択と copy。正解: 選んだ文字が青で塗られ、copy の文字が「The quick」。確認方法: log `PDFVIEWER SELECT page=0 from=0 to=8`、`PDFVIEWER COPY bytes=9 text="The quick"`、撮影。

## 合格
1〜4（3a を含む）の log。撮影の塗りの位置は人が見る。

## 注記
ws128-p004 の正常系。頁をまたぐ選択・語と行・Ctrl+A・指の長押しと handle・一致の規則と数・titlebar の無い時の窓の中の検索の欄は ws177-p040〜p043（host の試験 `plan/ws177/tests/host-pdf-find-l.sh`）。log の SELECT の行の後ろに `to-page=`、COPY の行の後ろに `unreadable=` が付く（ws177-p042）。

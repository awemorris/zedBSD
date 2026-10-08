---
id: apps.settings.printers
title: printer を足し、PDF を IPP と LPD で印刷し、Settings の Printers の頁で見る
status: active
areas: [settings, printing, compositor]
paths: [userland/desktop/printd/, userland/desktop/libkeiland-backend/print/, userland/desktop/wayland/printers-shell.c, userland/desktop/settings/page-printers.c, userland/desktop/pdfviewer/, userland/tests/printtest/]
machine: either
human: look
since: ws145
---

## 目的
desktop が printer を覚え（`~/.config/keiland/printers.conf`）、libkeiland に PDF を渡すと keiland-printd が IPP と LPD で printer に送り、Settings の Printers の頁に printer と job が出ることを確かめる（WS145 p002〜p004）。

## 準備
host で `plan/ws145/tests/mock-printers.py`（IPP は `/ipp/print` で名前「Mock Printer」、LPD）を target から届く address（QEMU なら 10.0.2.2）で。kei の printers.conf を消す。runner の試料 `/tmp/aat-samples/sample.pdf`。image に keiland-printd と printtest。

## 操作と確認
1. 操作: kei として `printtest add ipp ADDRESS IPP の port`、`printtest add lpd ADDRESS LPD の port raw`、3 秒後に `printtest list`。
   確認事項: 追加。正解: `PRINTTEST open printers=1`、`PRINTTEST result error=0` が 2 回、list に `printer id=1 protocol=1 … default=1 name=Mock Printer`（IPP の名前は printer に問い合わせた物）と id=2 の LPD。確認方法: printtest の出力。
2. 操作: `printtest print /tmp/aat-samples/sample.pdf 'AAT page'`（既定＝IPP）、`printtest print --printer=2 … 'AAT LPD'`。
   確認事項: 印刷。正解: `PRINTTEST done job=1 state=4`・`job=2 state=4`、host の mock が受けた `ipp-1.pdf` と `lpd-1.data` が試料と同じ（SHA-256）。確認方法: printtest の出力、host の file。
3. 操作: kei として `pdfviewer /tmp/aat-samples/sample.pdf`、窓を click して Ctrl+P（File > Print）。
   確認事項: PDF Viewer の印刷（D6）。正解: `PDFVIEWER PRINT asked error=0`、`PDFVIEWER PRINT job=3 state=4`、mock の `ipp-2.pdf` が試料と同じ、窓に「Printed.」。確認方法: log、host の file、撮影。
4. 操作: kei として `printtest edit LPD の id Basement raw2`（ws177-p025、printer の名前と queue を変える）。
   確認事項: 変更。正解: `PRINTTEST result error=0`、list の LPD の printer が `path=raw2 … name=Basement`。確認方法: printtest の出力。
5. 操作: Settings の Printers の頁を開く。
   確認事項: 頁。正解: Mock Printer に「Default」、Basement（LPD、raw2）、各行に Edit、Add a Printer の form、Print Jobs に 3 つの Done。確認方法: 撮影。

## 合格
1〜5 の正解。

## 注記
助け: `plan/tools/aat/scenarios/helpers_printers.py`（前後に printers.conf を消す）。

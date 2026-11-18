---
id: apps.calendar.navigate
title: Calendar の日付を keyboard で移り、月をめくる
status: active
areas: [calendar]
paths: [userland/desktop/calendar/]
machine: either
human: look
since: ws155
---

## 目的
Calendar が今日を正しく知り、日付の移動・月めくり・今日への戻りが動くことを確かめる。

## 準備
App Home から Calendar を開く。target の日付を `date +%Y-%m-%d` で読む。

## 操作と確認
1. 操作: 開いた時の log を読む。
   確認事項: 今日。正解: `CALENDAR READY today=YYYY-MM-DD` が target の日付。確認方法: log と `date`。
2. 操作: 窓の中を click して焦点を移し、右矢印。
   確認事項: 選択。正解: `CALENDAR SELECT date=` が翌日。確認方法: log、撮影。
3. 操作: Page Down。
   確認事項: 月。正解: `CALENDAR FLIP from=… to=…`、翌月の表。確認方法: log、撮影（めくりの絵は人が見る）。
4. 操作: `t`。
   確認事項: 今日へ。正解: `SELECT date=` が今日（か `FLIP … to=` が今日）。確認方法: log。

## 合格
1〜4 の log。

## 注記
click は日付の格子に当たると `SELECT` が出る。予定の無い所に当たると新規作成の画面も開く（`CALENDAR EDIT new`、ws155-p006）ので、helper は panel の Cancel で閉じてから右矢印を押す（新規作成の画面は欄に焦点を置かないので、閉じなくても矢印は日を移す）。2 の右矢印の前の click が選んだ日で、右はその翌日。今日の確かめは 4 で。

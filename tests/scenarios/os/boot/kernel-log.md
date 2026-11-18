---
id: os.boot.kernel-log
title: 起動の kernel の log に panic・fault が無い
status: active
areas: [kernel]
paths: [src/]
machine: either
human: none
since: ws159-p005
---

## 目的
起動の間に kernel が異常を記録していないことを確かめる。

## 準備
os.boot.session-up が通った。

## 操作と確認
1. 操作: kernel の log を読む（`/var/log/kernel.log` と `dmesg`、root）。
   確認事項: `panic`・`fatal`・`page fault` の行。正解: どれも無い。確認方法: 出力を grep し、全文を実行の記録に `dmesg.txt` として残す。

## 合格
3 つの語のどれも kernel の log に無い。

## 注記
5330 の ACPI と touchpad の行は os.acpi.tables-and-touchpad。

---
id: os.accounts.su-root-locked
title: root の password は lock されていて su できない
status: active
areas: [su, accounts, terminal]
paths: [userland/base/su/, userland/base/login/, Makefile]
machine: either
human: none
since: ws129-p004
---

## 目的
release の構成（`ZEDBSD_ROOT_LOCKED=y`）で root に su できないことを確かめる（UAT 6.3）。

## 準備
App Home から Terminal を開く。

## 操作と確認
1. 操作: `su -c true; echo rc=$? > /tmp/aat-work/su.txt` と打って Enter、password の問いに何かを打って Enter。
   確認事項: 結果。正解: `su: authentication failed` が出て、file の `rc=` が 0 でない。確認方法: file を読む、撮影。

## 合格
`rc=` が 0 でない。

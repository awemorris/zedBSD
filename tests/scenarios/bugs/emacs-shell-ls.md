---
id: bugs.emacs-shell-ls
title: Emacs の M-x shell で `ls /` が端末と同じ列の形で並ぶ（目視）
status: active
areas: [emacs, terminal]
paths: [userland/base/emacs/]
machine: either
human: look
since: BUG-242
---

## 目的
BUG-242（M-x shell で `ls /` の layout が崩れる）を撮る。

## 準備
AAT の image。

## 操作と確認
1. 操作: Terminal で `emacs -nw`、M-x shell、`ls /`。
   確認事項: shell の buffer の ls の出力。正解: 名前が揃った列で並び、崩れない（数は `ls / | wc -l` と同じ）。確認方法: 撮影 shell-ls。
2. 操作: C-x C-c、`yes`。
   確認事項: Emacs が終わる。正解: Terminal の prompt に戻る。確認方法: （確認なし）

## 合格
人が撮影を見て判断（needs-person）。

## 注記
崩れていたら BUG-242 の再現（TERM・COLUMNS・tab の幅を ticket に）。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。

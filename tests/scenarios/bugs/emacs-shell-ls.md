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
1. 操作: Terminal で `emacs -nw`、M-x shell、`stty size; ls /`。
   確認事項: shell の buffer の stty と ls の出力。正解: `stty size` が `0 0`（Emacs が開いた pty は大きさを持たない。直す前は console の大きさ）、ls の名前が揃った列で並び、行が折り返さない（数は `ls / | wc -l` と同じ）。確認方法: 撮影 shell-ls。
2. 操作: C-x C-c、`yes`。
   確認事項: Emacs が終わる。正解: Terminal の prompt に戻る。確認方法: （確認なし）

## 合格
人が撮影を見て判断（needs-person）。

## 注記
原因（2026-10-09 P1）: 新しい pty の大きさが console の text の大きさ（例 160 列）で、ls がその幅で並べ、Emacs の窓で折り返していた。kernel（`src/kern/tty.c`）で新しい pty の大きさを 0 にした。崩れていたら BUG-242 を再び開く（`stty size` の値を ticket に）。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。

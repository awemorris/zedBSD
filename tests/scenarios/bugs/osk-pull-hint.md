---
id: bugs.osk-pull-hint
title: 画面 keyboard の角の引き出しが Notes と同じ扇形と文字（目視）
status: active
areas: [osk]
paths: [userland/desktop/wayland/keyboard.c]
machine: either
human: look
since: BUG-230
---

## 目的
BUG-230（引き出しの領域が真っ白な四角）の今の見え方を撮る。

## 準備
AAT の image。Text Editor に focus。

## 操作と確認
1. 操作: 右下の角で押し、離さずに対角線を 40・80・130 px 引き、各所で撮り、離す。
   確認事項: 角の hint。正解: `KWL OSK armed corner=flick`、離して `KWL OSK open kind=flick`。40 px は円だけ、80 px で「Keyboard」が現れ、130 px で文字と青い縁。確認方法: log、撮影 hint-short・hint-label・hint-ready。

## 合格
行が出て、人が撮影で白い四角でないこと・Notes の引き出しと同じ見え方を確かめる（needs-person）。

## 注記
T1-226b の所見「四分円の地は白っぽい」と比べる。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。

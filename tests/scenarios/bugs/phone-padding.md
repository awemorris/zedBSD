---
id: bugs.phone-padding
title: Phone の本体の周りに padding が無く、title bar との隙間が Settings と同じ（目視）
status: active
areas: [phone]
paths: [userland/desktop/phone/]
machine: either
human: look
since: BUG-218
---

## 目的
BUG-218 の前半（Phone の本体の周りの padding）を Settings と並べて撮る。

## 準備
AAT の image。Phone の連絡先 2 つを helper が置く。

## 操作と確認
1. 操作: Phone を開いて撮る。Phone を閉じ、Settings を開いて撮る。
   確認事項: 本体の端。正解: Phone の本体が窓の端まで、title bar との隙間が Settings と同じ。確認方法: 撮影 phone・settings。

## 合格
人が撮影を見て判断（needs-person）。

## 注記
後半（慣性 scroll の開始の遅れ）は touch pad の 2 本指で、実機の UAT。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。

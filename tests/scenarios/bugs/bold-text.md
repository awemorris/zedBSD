---
id: bugs.bold-text
title: app の太字（見出し・名前）が regular と同じ柔らかさで描かれる（目視）
status: active
areas: [fonts, settings, phone, files]
paths: [userland/desktop/libtruetype/]
machine: either
human: look
since: BUG-205
---

## 目的
BUG-205（太字の font の anti-alias が regular と違って美しくない）の今の見え方を撮る。英語と日本語の UI。

## 準備
AAT の image。Phone の連絡先 2 つを helper が置く。

## 操作と確認
1. 操作: Settings・Phone・Files を開いて撮る。
   確認事項: 太字の見出しと名前。正解: stem の太さが揃い、縁が regular と同じ柔らかさ、字が重ならない。確認方法: 撮影 settings・phone・files。
2. 操作: `keiland-settings set ui.language 1`（日本語）で Settings を開いて撮り、元に戻す。
   確認事項: 日本語の太字。正解: 同じ。`KWL LANGUAGE language=ja`。確認方法: log、撮影 settings-ja。

## 合格
人が撮影を見て判断（needs-person）。

## 注記
見た目の判定はユーザー。T1-218 の PNG（2026-10-06）と比べてよい。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。

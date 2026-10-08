---
id: bugs.phone-ime-flick
title: Phone の message の欄に IME と画面 keyboard の flick で日本語を入れる
status: active
areas: [phone, ime, osk]
paths: [userland/desktop/phone/, userland/desktop/libkeiland/ui/field.c, userland/desktop/wayland/keyboard.c]
machine: either
human: none
since: BUG-203
---

## 目的
BUG-203（Phone で日本語を入力できない）と BUG-204（画面 keyboard の日本語の flick が無視される）が今も起きないかを確かめる。

## 準備
AAT の image。helper が `~/Documents/Phone` に連絡先 2 つ（Ben・Aiko）を置き、`phone.backend` を 1（loopback）にし、入力方式を日本語（method 1）にする。終わりに全部戻す。

## 操作と確認
1. 操作: Phone を開き Ben を選び、message の欄を click、Alt+Space で日本語、`nihon`・Space・Enter（確定）、Alt+Space、Enter（送信）。
   確認事項: IME の日本語が欄に入り送られる（BUG-203）。正解: `PHONE SEND contact=0 channel=0 length=N error=0`、Ben の message の file に日本語（非 ASCII）。確認方法: log、file の中身、撮影 converting・ime-sent。
2. 操作: 画面 keyboard の flick の panel を開き、kana の面で「か」、濁点の key。
   確認事項: kana が欄に届く（BUG-204）。正解: `KWL OSK send via=commit text=が before=3`、`KWL OSK refused` が無い。確認方法: log、撮影 flick。
3. 操作: panel を閉じ、欄で Enter。
   確認事項: 「が」が送られる。正解: `PHONE SEND contact=0 channel=0 length=3 error=0`。確認方法: log。

## 合格
1・2・3 の全部。どれかが無ければ BUG-203 か BUG-204 の再現（fail、note に段）。

## 注記
Mailer・Calendar の欄は T1-260・T1-317 で確認済み（ticket）。実機の画面 keyboard の flick は touch screen の UAT。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。

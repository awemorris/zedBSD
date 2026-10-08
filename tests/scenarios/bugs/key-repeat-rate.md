---
id: bugs.key-repeat-rate
title: Settings の key の repeat の速さが app の repeat に効く
status: active
areas: [keyboard, compositor, terminal]
paths: [userland/desktop/libkeiland/ui/window.c, userland/desktop/settings-keys/settings-keys.c]
machine: either
human: none
since: BUG-191
---

## 目的
BUG-191（Settings の repeat の設定が効かない）と BUG-172（repeat が安定しない）のうち、設定が client の repeat に効くことを、key を 3 秒押し続けて入った文字の数で確かめる。

## 準備
AAT の image。helper が `keyboard.repeat.rate`・`delay` の元の値を覚えて戻す（delay は 400 に）。

## 操作と確認
1. 操作: rate 5 にして Terminal で `cat > /tmp/aat-work/repeat5.txt`、a を 3 秒押し続け（`aat-input key-down a`・`sleep 3000`・`key-up a`）、Enter、Ctrl+D。
   確認事項: 約 1 + 2.6×5 = 14 文字。正解: a の数が 6〜22。確認方法: file の中身、撮影 rate5。
2. 操作: rate 40 で同じ。
   確認事項: 約 105 文字。正解: a の数が rate 5 の 3 倍以上（70〜130 なら pass）。確認方法: file の中身、撮影 rate40。

## 合格
1・2 の数が範囲に入る。rate が効かなければ BUG-191 の再現（fail）。40 で頭打ち（QEMU）なら値を書いて needs-person。

## 注記
5330 の内蔵 keyboard（PS/2 の typematic）と repeat の安定の体感は実機の UAT。注入の keyboard は PS/2 の typematic を出さない。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。

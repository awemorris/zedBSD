---
id: desktop.language.lock-japanese
title: 表示の言語が日本語の時、lock の画面も日本語になる
status: active
areas: [compositor, i18n, greeter]
paths: [userland/desktop/wayland/greeter.c, userland/desktop/wayland/language.c, userland/desktop/locale/ja/wayland.tr]
machine: either
human: look
since: ws158-p003
---

## 目的
`ui.language` を日本語にすると、Super+L の lock の画面の文（パスワードなど）が日本語になることを確かめる（ws158-p003）。

## 準備
desktop（sessiond が始めた session。zdesktop は sessiond の無い時は lock しない: 解く者がいないため、greeter.c の `zwl_lock`）。

## 操作と確認
1. 操作: user で `keiland-settings set ui.language 1`。
   確認事項: 日本語。正解: `ZWL LANGUAGE language=ja`。確認方法: log。
2. 操作: Super+L、撮影、password `user`、Enter。
   確認事項: lock と解除。正解: `ZWL LOCK locked`・`ZWL LOCK unlocked`、撮影で日本語（パスワード）。確認方法: log、撮影。
3. 操作: `keiland-settings set ui.language 0`。
   確認事項: 英語に戻る。正解: `ZWL LANGUAGE language=en`。確認方法: log。

## 合格
1〜3 の正解、撮影の日本語。

## 注記
T1-207 の `tr-p003-guest.sh` は sessiond の無い zdesktop を単独で起動するので lock しない（`ZWL LOCK locked` が出ない）。その段をこのシナリオに移した（q809）。

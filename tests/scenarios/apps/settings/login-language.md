---
id: apps.settings.login-language
title: 管理者が Languages の頁で login の画面の言語を変える
status: active
areas: [settings, account-admin, i18n]
paths: [userland/desktop/settings/page-languages.c, userland/base/account-admin/]
machine: either
human: none
since: ws158-p004
---

## 目的
管理者（wheel）が自分の password で system の言語（`/etc/keiland/language`、login の画面の言語）を変えられることを確かめる。

## 準備
user（管理者）で Settings の Languages の頁。Login screen の card が出る。

## 操作と確認
1. 操作: 頁を下へ送り、Login screen の「日本語」（control 7）、password の欄（control 8）に `user`、Enter。
   確認事項: 変更。正解: `ZSETTINGS LANGUAGES system result request=N errno=0 system=1`、`/etc/keiland/language` が `ja`。確認方法: log、file。
2. 操作: 「English」（control 6）、password、Enter。
   確認事項: 戻す。正解: `errno=0`、file が `en`。確認方法: log、file。

## 合格
1・2 の正解。

## 注記
file が無かった時も、終わりは `en`（無いのと同じ English）。logout の後の greeter の言語は desktop.session.logout-login と UAT で見る。

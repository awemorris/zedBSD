---
id: apps.settings.manage-users
title: Manage users で利用者を足して消す
status: active
areas: [settings, accounts, account-admin, compositor]
paths: [userland/desktop/settings/page-users-admin.c, userland/desktop/wayland/system.c, userland/base/account-admin/]
machine: either
human: look
since: ws089-p026
---

## 目的
管理者が Settings から利用者を足し、自分の password の誤りで拒まれ、消せることを確かめる。

## 準備
kei は wheel。Settings の Users の頁。`aatuser` はいない。

## 操作と確認
1. 操作: Manage users の card の「Add User」を click。
   確認事項: form。正解: `ZSETTINGS USERS admin start mode=1`、User name・Full name・New password・Your password の欄。確認方法: log、撮影。
2. 操作: User name に英字を 40 字（`abcdefghijklmnopqrstuvwxyzabcdefghijklmn`）打ち、Esc で form を閉じてもう一度「Add User」。
   確認事項: 名前の欄は account-admin の上限 32 byte で止まる（ws177-p004）。正解: `ZSETTINGS USERS admin field=0 length=32` があり、`length=33` 以上が無い。確認方法: log、撮影（欄に 32 字）。
3. 操作: `aatuser`、Tab、`AAT User`、Tab、`aat-pass-1`、Tab、`wrong-pass`、Enter。
   確認事項: 拒否。正解: `ZSETTINGS USERS admin result … reason=bad-password`、`/etc/passwd` に `aatuser` が無い。確認方法: log、grep。
4. 操作: もう一度「Add User」、同じ値で Your password を `kei`、Enter。
   確認事項: 追加。正解: `ZSETTINGS USERS admin result request=N errno=0`、`/etc/passwd` に `aatuser`、一覧に出る。確認方法: log、grep、撮影。
5. 操作: 一覧の `aatuser` の行を click、「Remove」、Your password に `kei`、Enter。
   確認事項: 削除。正解: `… admin start mode=3`、`… admin result … errno=0`、`/etc/passwd` に `aatuser` が無い。確認方法: log、grep。

## 合格
2〜5 の正解。

## 注記
2 は ws177-p004 で足した。Add User は control 21（`ADMIN_START_FIRST` + 1）、Remove は 23、一覧の行は 100 から（新しい利用者は `/etc/passwd` の終わりなので最後の行）。失敗した時は os.accounts.account-admin の 5 の要領で root から消す。

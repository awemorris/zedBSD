---
id: os.accounts.account-admin
title: account-admin は呼び手・password・値を確かめる
status: active
areas: [account-admin, accounts]
paths: [userland/base/account-admin/, userland/base/login/]
machine: either
human: none
since: ws089-p026
---

## 目的
Settings の Manage users の裏の道具 `/usr/libexec/account-admin` が、呼び手（root でない wheel の者）、その password、名前・password の規則を確かめてから account の file を変えることを確かめる。

## 準備
user は wheel で password は `user`。要求は標準入力の行: 呼び手の password、操作、引数（add: 名前・full name・新しい password・`admin` か `user`、remove: 名前・`keep-home` か `remove-home`）。答えは 1 行 `ok` か `error 理由`。エージェントは root から `su user -c …` で user として流す。

## 操作と確認
1. 操作: `aatuser`（`AAT User`、`aat-pass-1`、user）の add を、違う呼び手の password で。
   確認事項: 答え。正解: `error bad-password`（2 秒ほど後）。確認方法: 出力。
2. 操作: 名前 `Bad Name` の add を正しい password で。
   確認事項: 答え。正解: `error bad-name`。確認方法: 出力。
3. 操作: password `short` の add。
   確認事項: 答え。正解: `error weak-password`。確認方法: 出力。
4. 操作: 正しい add。
   確認事項: 答えと file。正解: `ok`、`/etc/passwd` に `aatuser`、`/home/aatuser` がある。確認方法: 出力、root で grep・ls。
5. 操作: `aatuser` の remove（`remove-home`）。
   確認事項: 答えと file。正解: `ok`、`/etc/passwd` に `aatuser` が無い。確認方法: 出力、grep。
6. 操作: root のまま add を流す。
   確認事項: 答え。正解: `ok` でない（root は呼び手になれない）。確認方法: 出力。

## 合格
各操作の正解。

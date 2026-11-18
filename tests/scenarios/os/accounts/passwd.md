---
id: os.accounts.passwd
title: passwd で user の password を変えられ、短い password は拒まれる
status: active
areas: [passwd, accounts, terminal]
paths: [userland/base/passwd/, userland/base/login/]
machine: either
human: none
since: ws160-p001
---

## 目的
`passwd` の規則（8 文字以上）と変更を確かめる（UAT 6.4）。

## 準備
user の password は `user`。root で `/etc/shadow` を控える（最後に戻す）。Terminal を開く。

## 操作と確認
1. 操作: `passwd` と打って Enter、今の `user`、新しい `short` を 2 回。
   確認事項: 拒否。正解: 8 文字未満の理由が出て、user の `/etc/shadow` の行が変わらない。確認方法: 撮影、root で shadow の user の行を前と比べる。
2. 操作: `passwd`、今の `user`、新しい `aat-pass-1` を 2 回。
   確認事項: 変更。正解: `passwd: the password of user is changed.`、shadow の user の行が変わる。確認方法: 撮影、shadow の比較。
3. 操作: 控えた `/etc/shadow` を戻す（root）。
   確認事項: 元の password。正解: user の行が最初と同じ。確認方法: 比較。

## 合格
短い password が拒まれ、長い password で変わり、shadow が元に戻る。

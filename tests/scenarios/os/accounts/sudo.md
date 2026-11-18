---
id: os.accounts.sudo
title: sudo は user の password で通り、違う password を拒む
status: active
areas: [sudo, accounts, terminal]
paths: [userland/base/sudo/, userland/base/login/]
machine: either
human: none
since: ws160-p001
---

## 目的
user（wheel）が自分の password で root の命令を流せ、違う password が 3 回で拒まれることを確かめる（UAT 6.1・6.2）。

## 準備
user の password は `user`。App Home から Terminal を開く。`/tmp/aat-work`（誰でも書ける）がある。

## 操作と確認
1. 操作: Terminal に `sudo id -u > /tmp/aat-work/sudo.txt; echo rc=$? >> /tmp/aat-work/sudo.txt` と打って Enter、password の問いに `user`・Enter。
   確認事項: 結果。正解: file が `0` と `rc=0` の 2 行。確認方法: root で file を読む。
2. 操作: `sudo -k; sudo true; echo rc=$? > /tmp/aat-work/sudo-wrong.txt` と打って Enter、違う password・Enter を 3 回（各 2 秒ほど待たされる）。
   確認事項: 結果。正解: `rc=` が 0 でない。確認方法: file を読む。

## 合格
1・2 の正解。

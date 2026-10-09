---
id: apps.settings.change-password
title: Users の頁の Change Password のウィザードで password を変える（間違い・不一致・短い・正しい）
status: active
areas: [settings, accounts, compositor]
paths: [userland/desktop/settings/page-users-password.c, userland/desktop/settings/dialog.c, userland/desktop/wayland/system.c, userland/base/passwd/]
machine: either
human: look
since: ws160-p002
---

## 目的
Settings の password の変更（ws200-p001 から popup のウィザード: 今の password → 新しい password を 2 回 → Done）が、今の password の誤り・
不一致・短い password を拒み、正しい時に変えることを確かめる（UAT 6.7〜6.9、WS200 の UAT 8）。

## 準備
root で `/etc/shadow` を控える（最後に戻す）。Settings の Users の頁。

## 操作と確認
1. 操作: Password の card の Change Password を押して撮る。
   確認事項: popup。正解: 「Step 1 of 2」「Change your password」、Current password の欄、Next。log `ZSETTINGS USERS start flow=1`。確認方法: log、撮影。
2. 操作: `wrong-pass`、Enter（Next）。撮る。New password・again の 2 つの欄に `aat-pass-1` と `aat-pass-2`、Enter。
   確認事項: 不一致はその場で。正解: 「Step 2 of 2」、「The new password and its repeat differ.」、送られない（`USERS change request=` が無い）。確認方法: log、撮影。
3. 操作: 2 つの欄に `short`、Enter。
   確認事項: 短い。正解: 「Use at least 8 characters.」。確認方法: 撮影。
4. 操作: 2 つの欄に `aat-pass-1`、Enter。
   確認事項: 今の password の誤り。正解: `ZSETTINGS USERS change request=N`、busy の後 `ZSETTINGS USERS result request=N errno=…`（0 でない）、Step 1 に戻り「The current password is wrong.」、shadow が変わらない。確認方法: log、撮影、shadow の比較。
5. 操作: `kei`、Enter、2 つの欄に `aat-pass-1`、Enter。
   確認事項: 変更。正解: 「Your password is changed. Use the new one from now on.」と Done、`ZSETTINGS USERS result … errno=0`、shadow の kei の行が変わる。Done で popup が閉じる。確認方法: log、撮影、shadow の比較。
6. 操作: 控えた `/etc/shadow` を戻す（root）。

## 合格
1〜5 の正解（文の見えは needs-person）。

## 注記
Change Password は control 400。popup の中は Enter で次へ、Esc で閉じる。2 分触らないと今の password を消して Step 1 に戻る。

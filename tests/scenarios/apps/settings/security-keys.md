---
id: apps.settings.security-keys
title: Security Keys の頁と、鍵の無い機械で通る各ウィザードの step（Software Security Key・Add Key・Change PIN・Reset Key・サインインの方法）
status: active
areas: [settings, accounts, sessiond]
paths: [userland/desktop/settings/page-users-keys.c, userland/desktop/settings/page-users-pin.c, userland/desktop/settings/dialog.c, userland/desktop/wayland/system.c, userland/base/passkey/main.c]
machine: either
human: look
since: ws199-p004
---

## 目的
Settings の Security Keys の頁（ws199-p001 §3.1〜§3.5、p002）が 3 つの card を出し、popup のウィザードが鍵（CTAP2）の無い機械で通る step
（Software Security Key の設定、鍵の Insert の待ち、Reset の警告と password、PIN 不要・タッチ不要の選択と警告）を正しく出すことを確かめる。
鍵の登録・PIN の変更・Reset の実行・タッチは QEMU に鍵が無いので 5330 の UAT（ws199-p005）で見る。

## 準備
desktop に kei で login 済み。root で `/etc/passkey` を `/tmp/aat-passkey.saved` に控え（無ければ空と記録）、kei の作り物の鍵の行を 1 つ書く
（どの鍵も持たない credential、`plan/ws172/tests/fido2-p003-guest.sh` の手順 1 と同じ）:
`umask 077; printf '# zedBSD passkey 1\nkei:1000:fido2:AQIDBA:pQECAyYgASFYIGBhYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5_IlggQEFCQ0RFRkdISUpLTE1OT1BRUlNUVVZXWFlaW1xdXl8:0:zedbsd.login:Desk key:2026-10-06\n' > /etc/passkey`。
kei で `/bin/settings security-keys` を開く（session の socket で、`fido2-p003-guest.sh` の手順 4 と同じ）。

## 操作と確認
1. 操作: 頁が出るのを待って撮る。
   確認事項: 3 つの card。正解: log `ZSETTINGS PAGE security-keys`。撮影で上から「Software Security Key」（Set Up PIN）、「Security keys」（「One security key is registered.」、行「Desk key」と Remove、Add Key・Change PIN・Reset Key）、
   「Sign in with a security key」（「PIN and touch」に印、「Touch only (no PIN)」「No PIN, and no touch to unlock」）。確認方法: log、撮影（人が見る）。
2. 操作: Software Security Key の Set Up PIN を押し、`kei`、Next、PIN の 2 つの欄に `135790`、Set PIN、Done。各 step を撮る。
   確認事項: password → PIN の 2 step と完了。正解: 「Step 1 of 2」「Step 2 of 2」、PIN の欄は数字だけ、log `ZSETTINGS KEYS pin start remove=0`・`KEYS pin request=`・`KEYS pin result … errno=0`、
   `/etc/passkey` に `kei:1000:pin:` の行。処理中は popup が灰色で操作できない（撮れれば）。確認方法: log、撮影、root で `/etc/passkey` を見る。
3. 操作: Add Key を押し、`kei`、Next。「Your security key」の step を撮り、Cancel。
   確認事項: 鍵の無い時の待ち。正解: 「Step 2 of 5」、「Looking for your security key...」の後に「Plug in your security key, or hold it to the reader.」と Check Again・Back、
   log `KEYS key start flow=1`・`KEYS info result errno=0 count=0`。Cancel で popup が閉じる。確認方法: log、撮影。
4. 操作: Change PIN を押し、step を撮り、Cancel。
   確認事項: 同じ待ち。正解: 「Step 1 of 2」、「Plug in your security key …」、log `KEYS key start flow=5`・`KEYS info result errno=0 count=0`。確認方法: log、撮影。
5. 操作: Reset Key を押して撮る → Continue → password の step を撮る → Cancel（送らない）。
   確認事項: Reset の警告と password。正解: 「Step 1 of 4」で「Resetting erases everything on the key …」と Continue、「Step 2 of 4」で Password の欄と「Reset Key」、log `KEYS key start flow=6`、
   `KEYS key request=` は出ない。確認方法: log、撮影。
6. 操作: 「Touch only (no PIN)」を押して撮る → Continue → `kei` → Change → Done。
   確認事項: 弱い方への警告と保存。正解: 「Step 1 of 2」の警告「Anyone who has your security key can sign in to this computer with a touch, without its PIN.」、
   log `KEYS key start flow=7`・`KEYS key result request=N errno=0 flow=7`、`/etc/passkey` に `kei:1000:options:` で `key-pin=0:key-touch=1` の行、card の印が 2 つ目へ。確認方法: log、撮影、`/etc/passkey`。
7. 操作: 「No PIN, and no touch to unlock」を押して撮る → Continue → `kei` → Change → Done。
   確認事項: 置いたままの鍵の警告。正解: 警告に「While your key stays plugged in (or lies on the reader), anyone at this computer can unlock it with a swipe.」、options の行が `key-pin=0:key-touch=0`。確認方法: 撮影、`/etc/passkey`。
8. 操作: 「PIN and touch」を押して撮る → `kei` → Change → Done。
   確認事項: 強い方へは警告無し。正解: 最初の step が password（「Step 1 of 1」、警告無し）、保存の後 `kei:1000:options:` の行が無くなる（既定）。確認方法: 撮影、`/etc/passkey`。
9. 操作: 「Touch only (no PIN)」で Continue の後、password に `wrong` → Change。
   確認事項: password の誤り。正解: password の step に戻り「The password is wrong.」、`/etc/passkey` は変わらない。確認方法: 撮影、`/etc/passkey`。Cancel で閉じる。
10. 操作: 控えた `/etc/passkey` を戻す（root）。

## 合格
1〜9 の正解（文と配置の見えは needs-person）。

## 注記
Settings の log は `ZSETTINGS` で始まる。flow の番号は 1 Add Key、5 Change PIN、6 Reset Key、7 サインインの方法。
鍵が 1 本も登録されていない時、「Sign in with a security key」の 3 つは灰色で押せない。popup は 2 分触らないと password を消して password の step に戻る。

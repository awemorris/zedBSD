---
id: apps.settings.sign-in-methods
title: Users の頁の Sign-in Methods で方式を切り、login と lock の画面の方式が変わる（console・SSH は password のまま）
status: active
areas: [settings, accounts, sessiond, greeter, lock]
paths: [userland/desktop/settings/page-users-password.c, userland/base/passkey/main.c, userland/base/passkey/record.c, userland/desktop/sessiond/auth.c, userland/desktop/wayland/greeter.c, userland/desktop/wayland/system.c]
machine: either
human: look
since: ws200-p001
---

## 目的
Sign-in Methods の切り替え（ws200-p001）で、外した方式が lock と login の画面に出ず、出されても受けず（`style-off`）、
password を外しても console・su・SSH は password で入れることを確かめる（WS200 の UAT 8）。QEMU に鍵は無いので、鍵の方式は作り物の鍵の行で表示だけを見る。

## 準備
desktop に kei で login 済み。root で `/etc/passkey` を `/tmp/aat-passkey.saved` に控える（無ければ空と記録）。
Settings → Security Keys で Software Security Key の PIN `135790` を設定し、一度 password で lock を解いておく（PIN は password の後に出る）。
Settings の Users の頁を開く。

## 操作と確認
1. 操作: Sign-in Methods の card を撮る。
   確認事項: 3 つの switch。正解: Password・PIN (Software Security Key) は on、Security Key は off で灰色（「No key registered …」）。Password の下に「The password or a security key must stay on.」（鍵が無いので外せない、灰色）。確認方法: 撮影（人が見る）。
2. 操作: PIN の switch を押し、Step 1 of 1 を撮る → `kei` → Change → Done。
   確認事項: password で確かめて PIN を外す。正解: log `ZSETTINGS USERS start flow=2 method=2`・`USERS methods request=N methods=5`・`USERS methods result request=N errno=0`、`/etc/passkey` に `kei:1000:options:methods=password,fido2:key-pin=1:key-touch=1`、switch が off。確認方法: log、撮影、`/etc/passkey`。
3. 操作: Super+L、下から上へドラッグして card を出す。撮る。
   確認事項: PIN が出ない。正解: `KWL GREETER styles=1`（password だけ）、PIN の pill が無い。password で解除。確認方法: log、撮影。
4. 操作: root で guest の端末から、外した PIN の解除を直に試す（`printf 'auth\nkei\npin\n135790\n' | /sbin/passkey`）。
   確認事項: 受けない。正解: `fail style-off`。確認方法: 出力。
5. 操作: Users の頁で PIN の switch を押し、`kei` → Change → Done。もう一度 Super+L とドラッグ。
   確認事項: PIN が戻る。正解: `/etc/passkey` の options の行が無くなる（既定）、`KWL GREETER styles=3`、PIN で解除できる。確認方法: log、`/etc/passkey`。
6. 操作: root で作り物の鍵の行を足す（`tests/scenarios/apps/settings/security-keys.md` の準備の行を `>>` で）。Settings を開き直し、Users の頁の card を撮る。
   確認事項: 鍵がある時。正解: Security Key の switch が on で押せる、Password も押せる（鍵が on なので）。確認方法: 撮影。
7. 操作: Password の switch を押す → 警告（Step 1 of 2、console の文）を撮る → Continue → `kei` → Change → Done。
   確認事項: password を外す。正解: 警告に「sign in on the console with your password」、`methods=6`、`/etc/passkey` に `methods=pin,fido2`、Security Key の switch が灰色（最後の 1 つ）。確認方法: log、撮影、`/etc/passkey`。
8. 操作: Super+L とドラッグ。
   確認事項: password が出ない。正解: `KWL GREETER styles=6`（PIN と鍵）、Password の pill が無い。PIN `135790` で解除できる（login の後なので PIN が出る）。確認方法: log、撮影。
9. 操作: SSH（または serial の console）で kei に password で login し、`su -c` で root になる。root で控えた `/etc/passkey` を戻す。
   確認事項: console・SSH・su は password のまま。正解: login と su ができる。`/etc/passkey` を戻すと次の lock で password が出て解ける。確認方法: 出力、log。

## 合格
1〜9 の正解（文と配置の見えは needs-person）。

## 注記
Sign-in Methods の switch は control 410（Password）・411（PIN）・412（Security Key）、Change Password は 400。methods の bit は 1 password、2 PIN、4 鍵。

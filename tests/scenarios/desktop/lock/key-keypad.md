---
id: desktop.lock.key-keypad
title: lock と login の画面の PIN の keypad（数字・ABC）と、鍵の無い時の鍵のモードの問い合わせ
status: active
areas: [compositor, lock, greeter, sessiond]
paths: [userland/desktop/wayland/greeter.c, userland/desktop/wayland/lock-key.c, userland/desktop/wayland/lock-keypad.c, userland/desktop/sessiond/auth.c, userland/base/passkey-fido2/main.c]
machine: either
human: look
since: ws199-p003
---

## 目的
lock と login の画面で、欄が PIN を取る間はすぐ下に keypad が出て押して打てること（Software Security Key は数字だけ、セキュリティキーの PIN は ABC で英字）、
画面が出た時と card が出た時に鍵の持ち主を問い（KEYOWNER）、鍵の無い機械では何もしないことを確かめる（ws199-p001 §3.6〜§3.8）。
鍵（CTAP2）は QEMU に無いので、鍵の自動のモード・タッチ・0.5 秒は 5330 の UAT（ws199-p005）で見る。

## 準備
desktop。Settings → Security Keys → Software Security Key で PIN（例 `135790`）を設定済み。password でもう一度 lock を解いておく（PIN は password の後に出る）。

## 操作と確認
1. 操作: Super+L、画面の下から上へドラッグして card を出す。
   確認事項: card と鍵の問い合わせ。正解: `KWL LOCK swipe via=pointer`、続いて `KWL GREETER key owner asked error=0` と `KWL GREETER key owner error=0 found=0 … action=0`（鍵が無いので何もしない。reason は `no-key`）。PIN が既定の欄。確認方法: log。
2. 操作: なし（card が出たまま）。
   確認事項: 数字の keypad。正解: `KWL GREETER keypad keys=12 letters=0 x=… y=… right=… bottom=…`（欄のすぐ下）。左下は「OK」（ABC は無い）。確認方法: log、撮影（人が見る: 欄のすぐ下に 1〜9、OK・0・←）。
3. 操作: keypad の数字を押して PIN を打つ（keys は 3 列 4 段の格子: 列の幅は (right−x−12)/3、段の高さは 40、間は 6。`1` は左上、`0` は最下段の中央）。最後に `OK` を押す。
   確認事項: keypad で打った PIN で解除。正解: `KWL GREETER keypad press action=1` が 6 回、`action=3`（OK）、`KWL LOCK unlocked`。確認方法: log。
4. 操作（鍵を登録した account だけ。QEMU では鍵を登録できないので 5330 の UAT で。QEMU では飛ばす）: もう一度 Super+L とドラッグ、方式の選択で Security Key を押す。
   確認事項: 鍵の欄と英字の keypad。正解: `KWL GREETER style=4 via=choice`、`KWL GREETER keypad keys=12 letters=0`、欄の hint は「Security key PIN」。左下の `ABC` を押すと `KWL GREETER keypad keys=30 letters=1`。確認方法: log、撮影（人が見る: q〜p・a〜l・↑ z〜m ←・123 OK）。
5. 操作: もう一度 Super+L とドラッグ（PIN の欄と数字の keypad）、方式の選択で Password を押す。
   確認事項: keypad が消える。正解: `KWL GREETER style=1 via=choice` と `KWL GREETER keypad none`。password を打って Enter で `KWL LOCK unlocked`。確認方法: log。
6. 操作: Log Out し、login の画面を待つ。`/var/log/greeter.log` に `KWL GREETER key owner asked` が出るまで（styles の答えの後、1 秒ほど）何も打たない。その後 password で login する。
   確認事項: login の画面でも鍵の持ち主を問い、鍵が無ければ何もしない。正解: `KWL GREETER key owner asked error=0` と `KWL GREETER key owner error=0 found=0 … action=0`。password で login できる。確認方法: log。
   注記: 画面が出てすぐ（styles の答えの前）に password と Enter を打つと、login が先に送られ、その間と login の後は持ち主を問わない（login を待たせないための製品の振る舞い）。T1-523 の FAIL はこれ。

## 合格
1〜3・5・6 の正解（4 は鍵を登録した機械だけ）。keypad の配置（欄のすぐ下、card の内側、時計と重ならない）は撮影を人が見る。

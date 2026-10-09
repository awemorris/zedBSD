<!-- awesome-plan project=zedbsd record=ws199-p002 -->
# ws199-p002: PIN 不要・タッチ不要の設定（設計の i04）

Status: planned（2026-10-10 Q1。P1 が次に着手）
Parent: [WS199](../ws.md) ・設計: [phase001](../phase001/phase.md) §2・§4.3・§4.4・§11 の R4・R6

## ゴール
- account ごとの設定 `<name>:<uid>:options:methods=..:key-pin=0|1:key-touch=0|1` が /etc/passkey にあり、passkey・passkey-fido2 がそれで判定する。
- Settings の Security Keys の頁に radio（PIN＋タッチ／タッチだけ／どちらも不要）。変更は password で確かめ、警告「鍵を持つ人は誰でも…、挿したまま・reader に置いたままだと機械の前の人は誰でも解除できる」を出す。
- login（greeter）は常にタッチ。タッチ不要は lock の解除だけ。

## すること・やり方
1. passkey: options の行の読み書き（無い時は既定 methods=全部・key-pin=1・key-touch=1、key-touch=0 は key-pin=0 の時だけ、不正は既定）、`enrolled` の答えに `options=`、新しい操作 `set-options`（password）と `auth-fido2`（word・name・login|unlock・鍵の PIN（空可））。WS200 は methods を、WS199 は key-pin・key-touch を変え、他の field は読んだまま書き戻す。
2. passkey-fido2: PIN が空で key-pin=0 なら PIN token 無し・UV を求めない。up=false は unlock かつ key-touch=0 の時だけ。署名の検査の flag も同じ条件だけ外す。
3. sessiond: AUTH の fido2 は `login`、UNLOCK の fido2 は `unlock` で送る。数えは答えの語で（bad-secret・cloned・署名の不一致は数える、timeout・canceled・no-key・抜けは数えない、R4）。`pin-required`・`cloned` はそのまま通す（R6）。
4. backend・compositor・libkeiland・Settings: set-options の経路と radio・警告の popup（dialog.c）。
5. 最後の鍵を消したら key-pin=1・key-touch=1 に戻す。security.md に規則。

## 確かめ
- host 試験: passkey の options の読み書き、passkey-fido2 の wire の検査の緩め（許さない組の UP・UV 無しを拒む）、sessiond の数え。build warning 0。
- 5330 での前提（ユーザーが fidoctl で流す、Q1 が手順を渡した）: `fidoctl -s assert` が触れずに flags 0x00 を返すか。

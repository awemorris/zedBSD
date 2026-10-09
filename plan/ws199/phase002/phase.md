<!-- awesome-plan project=zedbsd record=ws199-p002 -->
# ws199-p002: PIN 不要・タッチ不要の設定（設計の i04）

Status: cleared（2026-10-10 Q1 判定: 実装と host 試験（passkey・fido2・sessiond-auth・sessiond-keys）PASS、build warning 0、merge 77a40b51f。残り: passkey の options の読み書きの host 試験（再開点 (d)、p004 で）。実機は p005）
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

## 進み（P1）

| 日 | 内容 | 検証 |
| --- | --- | --- |
| 2026-10-10 | passkey: record.c に `passkey_options_*`（読み・書き・既定、重複・不正・key-touch=0 で key-pin=1 は既定）、`enrolled` の答えに ` key-pin=0|1 key-touch=0|1`、`set-options NAME PASSWORD PIN TOUCH`（鍵の無い account は not-enrolled、既定なら行を消す、methods は書き戻す）、`auth-fido2` を passkey-fido2 へ。passkey-fido2: `auth-fido2 NAME login|unlock PIN` で options を読み、空の PIN は key-pin=0 の時だけ（UV を求めない）、up=false は unlock かつ key-touch=0 の時だけ、検査の flag も同じ。最後の鍵を消すと key の options を既定へ。sessiond: AUTH/UNLOCK の fido2 は auth-fido2（login/unlock）、前もって数えず答えの語で数える（bad-key-pin・bad-secret・cloned・key-locked・key-replug だけ数え、他は遅れ無し、R4）、cloned・pin-required は写さず通す（R6）、`SETOPTIONS PIN TOUCH` + password 行。backend: ENROLLED の key-pin・key-touch、`kl_backend_session_set_options`。compositor: account の set_key_options（request 10）と options の event（9、enrolled の前）。libkeiland: `kl_system_account_key_options`・`_set_key_options` | zedBSD の build（wayland・settings・sessiond・passkey・passkey-fido2）warning 0。host 試験 PASS: passkey-host-test、fido2-host-test、sessiond-auth-host-test（cloned の期待を R6 に直した）、plan/ws199/tests/sessiond-keys-host-test.sh（SETOPTIONS、fido2 の no-key は数えず即答、bad-key-pin は数えて遅れ） |

| 2026-10-10 | Settings: Security Keys の頁に card「Sign in with a security key」（3 つの選択、鍵が 0 本なら灰色、KEY_OPS の無い所は出さない）、弱い方へは警告（Touch only / No PIN no touch で文を分ける）→ password、強い方へは password だけ → `kl_system_account_set_key_options` → Done。plan/ws089/tests/host-kl-system.c に options の stub（HOST_KEY_OPTION）。security.md に「How a key signs in」 | zedBSD の settings の build warning 0、host の renderer で選択・警告・Done（build/p1-ws199/opt-*.png）、style-check |

## 再開点（P1、2026-10-10）

- (a)〜(c) は済み（上の表）。残りは (d) だけ。以下は元の記述。
- 残り: (a) Settings の Security Keys の頁に radio の card「Sign in with a security key」（`kl_system_account_key_options` で今の値、鍵が 0 本なら灰色、KL_SYSTEM_HAS_KEY_OPS が無ければ出さない）。押すと popup（dialog.c）: 弱い方へは警告（Touch only:「Anyone who has your security key can sign in to this computer with a touch.」、No PIN no touch to unlock: それに「While your key stays plugged in (or lies on the reader), anyone at this computer can unlock it with a swipe.」）＋ password、強い方へは password だけ → `kl_system_account_set_key_options` → Done。flow を `SE_KEYS_FLOW_OPTIONS` として page-users-keys.c に足す。(b) plan/ws089/tests/host-kl-system.c に `kl_system_account_key_options`・`_set_key_options` の stub。(c) security.md に options の規則（login は常にタッチ、PIN 不要の人は置きっぱなしの NFC・挿しっぱなしで誰でも入れる、R4 の数え）。(d) passkey の options の読み書きの host 試験（plan/ws199/tests に小さく）。
- (a) まで済めば cleared 候補（T1 は p004 でまとめて、5330 は p005）。

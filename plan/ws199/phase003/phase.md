<!-- awesome-plan project=zedbsd record=ws199-p003 -->
# ws199-p003: ログイン・ロック画面の鍵のモードと keypad（設計の i05）

Status: in-progress（2026-10-10 P1）
Parent: [WS199](../ws.md) ・設計: [phase001](../phase001/phase.md) §3.6〜§3.8・§4.2・§11 の R2・R5・R10

## ゴール
- greeter: 鍵を挿す・reader に当てると、鍵の持ち主の user を選び鍵のモードに入る。key-pin=1 は PIN の欄＋すぐ下の keypad →タッチ、key-pin=0 はタッチだけ。抜くと password の欄に戻る。
- lock: swipe・key で card が出た後に鍵があれば鍵のモード。key-pin=0・key-touch=0 なら「Checking your security key…」で解除、成功でも最低 0.5 秒表示してから解く。card が無い時の鍵の出入りは何もしない。sleep に入る時に card を閉じる。
- keypad: 数字 3×4 と「ABC」で英字。Software Security Key の欄は数字だけ。

## すること・やり方
1. sessiond・passkey: `KEYOWNER`（鍵が持つ credential の持ち主の名前、`user=`・`none`・`many`）。policy に触れない（数えない・signed_in を立てない・遅れ無し、R2）。1 秒に 1 回まで（R10、間隔と「最後の 1 つ」は compositor が持つ）。session からは owner を固定。
2. compositor の greeter.c: keys_changed（p001 の i03 で作った鍵の出入りの事象）で KEYOWNER、鍵のモードの画面、「Use your password」の link、時間切れは自動で繰り返さず「Try again」。
3. lock: card が出た時と出ている間の鍵の出入りで KEYOWNER。0.5 秒の最低の表示。sleep.c で card を閉じ、待っている fido2 の要求に CANCEL（R5）。
4. keypad: greeter が PIN の欄の下に描く（物の keyboard も効く）。
5. AAT の scenario（鍵の無い QEMU で描ける所: keypad、「Use your password」）。

## 確かめ
- host 試験（greeter の鍵のモードの状態の遷移、0.5 秒、抜けで戻る、card の無い時は何もしない）。build warning 0。QEMU に CTAP2 の鍵は無いので鍵の振る舞いは 5330 の UAT。

## 進み（P1）

| 日 | 内容 | 検証 |
| --- | --- | --- |
| 2026-10-10 | (1) KEYOWNER の下側: passkey `key-owner NAME|-`、passkey-fido2 は全 account（`-`）か名指しの account の credential を account ごとの group にし、helper が 1 本の鍵（USB か reader に在る card、待たない）に silent（up=false・UV 無し）で group ごとに問う（allowList は鍵の maxCredentialCountInList ごと、言わない鍵は 1 つずつ、2 つ目の group で止める）、最初の group の答えの署名をその account の公開鍵で確かめる（合わなければ none）。答え `ok uid=N user=NAME key-pin key-touch card`、`fail none|many-owners|no-key|many-keys`。sessiond `KEYOWNER`（greeter は `-`、session は自分の user だけ）、policy に触れない（数えない・遅れ無し・signed_in 無し、R2）、sessiond 全体で 1 秒に 1 回、越えたら `ERROR busy`（R10）、答えの名前を passwd で引き直す。**p002 の不具合も直した**: libpasskey の `pk_verify_assertion` が required_flags に関係なく UP を必須にしていたので、unlock の key-touch=0（up=false）が必ず `bad-secret` で数えられていた → UP も required_flags に従う（fidoctl の `-s` の検査も同じ理由で通らなかった） | zedBSD の build（passkey・passkey-fido2・sessiond）warning 0。host 試験 PASS: libpasskey-host-test（verify の UP の組を足した）、fido2-host-test（owner の message の解析）、plan/ws199/tests/sessiond-keys-host-test.sh（KEYOWNER: session・greeter・1 秒・none・PIN を出さない）、sessiond-auth-host-test、passkey-host-test。commit 1b1861597 |

再開点: (2) backend `kl_backend_session_key_owner` と compositor の配線 → (3) greeter・lock の鍵のモード（純粋な状態は新しい file にして host 試験）→ (4) keypad → (5) sleep → (6) AAT → p002 (d)。

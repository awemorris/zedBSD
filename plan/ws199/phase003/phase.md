<!-- awesome-plan project=zedbsd record=ws199-p003 -->
# ws199-p003: ログイン・ロック画面の鍵のモードと keypad（設計の i05）

Status: planned（2026-10-10 Q1。p002 の後）
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

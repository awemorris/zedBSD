<!-- awesome-plan project=zedbsd record=ws199-p004 -->
# ws199-p004: 試験の残りと T1 の依頼（設計の i06）

Status: planned（2026-10-10 Q1。p003 の後）
Parent: [WS199](../ws.md) ・設計: [phase001](../phase001/phase.md) §7

## ゴール
- host 試験の残りが揃い、style-check が通り、T1 の AAT を 1 回の依頼で流す（使用量の節約、2026-10-10 ユーザーと Q1）。

## すること・やり方
1. host 試験の残り（§7: libpasskey の reset・status、passkey の request と options、passkey-fido2 の wire、sessiond、Settings のウィザードの純粋な関数、greeter の鍵のモード）。
2. style-check（変えた file）、build warning 0。
3. T1 の依頼の行を 1 つ（番号は Q1）: AAT の image で Security Keys の頁・各ウィザードの鍵の無い step（Insert の待ち・Software Security Key・一覧・radio と警告）・ロック画面と greeter の keypad の PNG、回帰 `plan/ws172/tests/fido2-p003-guest.sh`・passkey-p002-guest.sh・desktop.lock.swipe-card・wheel-card。
4. 5330 の UAT の一覧を ws.md と plan/beta2.md の「次の UAT」に。

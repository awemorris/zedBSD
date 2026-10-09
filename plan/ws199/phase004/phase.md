<!-- awesome-plan project=zedbsd record=ws199-p004 -->
# ws199-p004: 試験の残りと T1 の依頼（設計の i06）

Status: cleared 候補・test-wait（T1-（Q1）、Q1 が番号を振る。WS200 p001 の分も同じ行に足す）（2026-10-10 P1）
Parent: [WS199](../ws.md) ・設計: [phase001](../phase001/phase.md) §7

## ゴール
- host 試験の残りが揃い、style-check が通り、T1 の AAT を 1 回の依頼で流す（使用量の節約、2026-10-10 ユーザーと Q1）。

## すること・やり方
1. host 試験の残り（§7: libpasskey の reset・status、passkey の request と options、passkey-fido2 の wire、sessiond、Settings のウィザードの純粋な関数、greeter の鍵のモード）。
2. style-check（変えた file）、build warning 0。
3. T1 の依頼の行を 1 つ（番号は Q1）: AAT の image で Security Keys の頁・各ウィザードの鍵の無い step（Insert の待ち・Software Security Key・一覧・radio と警告）・ロック画面と greeter の keypad の PNG、回帰 `plan/ws172/tests/fido2-p003-guest.sh`・passkey-p002-guest.sh・desktop.lock.swipe-card・wheel-card。
4. 5330 の UAT の一覧を ws.md と plan/beta2.md の「次の UAT」に。

## 進み（P1、2026-10-10）

| 内容 | 検証 |
| --- | --- |
| (1) host 試験の残り（設計 §7）: libpasskey の authenticatorReset（`plan/ws161/tests/libpasskey-ctap2-host-test.c` の `run_reset`: 窓の外は NOT_ALLOWED で消さない、触れる待ちの間の CTAPHID CANCEL で KEEPALIVE_CANCEL（0x2d）を返し消さない、受けると credential と PIN が消える）。passkey-fido2 の検査の flag の決め方を `fido2_auth_flags`（wire.c、純粋）に出し、main_auth はそれを呼ぶ（振る舞いは同じ）。`plan/ws172/tests/fido2-wire-host-test.c` に全部の組（PIN＋タッチ・タッチだけ・どちらも無し × login・unlock × PIN の有無、key-touch=0 だけの不正な組）。Settings のウィザード `plan/ws199/tests/settings-keys-host-test.{c,sh}`（新、page-users-keys.c を include して Settings の host の build と link）: 名前の拒否と既定の名前、Add Key の step（password → Insert → 名前 → PIN → Touch → Done、password の保持と消去）、busy の間の Enter と 2 つ目のウィザードの拒否、2 分の idle で password を消す（busy の間は消さない）、新しい PIN の 2 回の照合、弱い方の警告と Back、強い方は password だけ。passkey の request と options は p003 の中で（passkey-options-host-test）、sessiond の KEYINFO・KEYPIN・KEYRESET・verified・replug・KEYOWNER の 1 秒は sessiond-keys-host-test、greeter の鍵のモードは lock-key-host-test（どれも p002・p003 で済み）。KEYOWNER の署名の照合は libpasskey の verify（required_flags 0）で、passkey-fido2 の全体の流れは 5330 | libpasskey-host-test・fido2-host-test・settings-keys-host-test PASS、passkey・passkey-options・sessiond-keys・sessiond-auth・lock-key の host 試験 PASS |
| (2) style-check: WS199 の前（5903f7548^）と比べて増えた所を直した（passkey-fido2/main.c 2、passkey/main.c 2、sessiond/auth.c 1、settings/dialog.c 1: 閉じ括弧の後の段落と式の Boolean）。WS199 で変えた 48 の C の file で増えた所は 0。build: zedBSD の passkey・passkey-fido2・sessiond・settings・wayland・fidoctl は warning 0、Linux の Keiland（`make -f userland/desktop/keiland-linux.mk all`、greeter・lock-key・lock-keypad・sleep・system・dialog・page-users-keys・session-none を含む）は warning 0。FreeBSD は host で build できないので T1 の依頼に | `git diff --check`、build の log `build/p1-p004/make-zedbsd.log`・`make-linux.log`（warning 0） |
| (3) T1 の依頼の行（`plan/agents/T1/requests.md` の末尾、「T1-（Q1）」）: 新しい AAT `tests/scenarios/apps/settings/security-keys.md`（頁と鍵の無い step、作り物の鍵の行で options の radio と警告）、`desktop.lock.key-keypad`、回帰 swipe-card・wheel-card・fido2-p003-guest.sh・passkey-p002-guest.sh、FreeBSD の guest の build | 未実行 |
| (4) 5330 の UAT の一覧（U1〜U13）を [ws.md](../ws.md) に | — |

commit: 40de43cab（試験と style の直し）、この記録と scenario と依頼の行は次の commit。

再開点: T1 の結果を Q1 が判定（PASS で cleared、FAIL は直しの Queue）。fido2-p003-guest.sh・passkey-p002-guest.sh は p003 の keypad と KEYOWNER の後に未実行なので、撮影の比較や手順が合わなければ P1 が script を直す。

<!-- awesome-plan project=zedbsd record=ws002-p014 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws002/phase014/phase.md`

親: [ws002](https://github.com/awemorris/zedBSD/issues/3)

# ws002-p014: getty, login sessions, and respawn

WSID: `ws002`

Phase ID: `p014`

Status: complete baseline

Parent WS: [WS002](https://github.com/awemorris/zedBSD/issues/3)

## Objective and design

Provide supervised `getty`, `login`, authentication/session setup, controlling
terminal ownership, logout, bounded respawn, and crash-loop protection without
making init dependent on shell internals.

## Acceptance and result

Installed-image QEMU acceptance reached login, entered and exited an interactive
shell, and exercised respawn. The shared case index is
[WS002 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/tests/README.md).

## Interruption record

Not interrupted. Authentication-policy expansion is not implied by the
completed console-session baseline.

## Completion conditions

- Supervised `getty` reaches `login` with correct terminal/session ownership.
- Successful login, failed authentication, logout, respawn, and crash-loop cases pass.
- The installed QEMU system returns to a usable login prompt after session exit.

<!-- awesome-plan project=zedbsd record=ws002-p012 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws002/phase012/phase.md`

親: [ws002](https://github.com/awemorris/zedBSD/issues/3)

# ws002-p012: native init and service lifecycle

WSID: `ws002`

Phase ID: `p012`

Status: complete

Parent WS: [WS002](https://github.com/awemorris/zedBSD/issues/3)

## Objective and design

Implement the single native `/sbin/init`, dependency-ordered service startup,
reverse stop order, supervision, `/run/init.sock`, `/sbin/service`, required
`mount -a`, and orderly halt/poweroff/reboot. Runlevels and shell-executed
service definitions are intentionally absent.

## Acceptance and result

The installed QEMU system boots under PID 1, operates services, supervises
children, and shuts down through the native lifecycle. Failure/cycle/timeout
coverage is indexed in [WS002 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/tests/README.md); original detailed
gates are retained in the [legacy plan](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/history/phase011-019-legacy-plan.md).

## Interruption record

Not interrupted. fd 3 readiness was extended later by `ws002-p020`.

## Completion conditions

- Native PID 1 completes dependency-ordered boot and reverse-order shutdown.
- `/sbin/service` controls runtime services and persistent enablement as specified.
- Supervision, failure/cycle/timeout, mount, sync, halt, reboot, and poweroff cases pass.

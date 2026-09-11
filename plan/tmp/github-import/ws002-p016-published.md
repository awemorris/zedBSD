<!-- awesome-plan project=zedbsd record=ws002-p016 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws002/phase016/phase.md`

親: [ws002](https://github.com/awemorris/zedBSD/issues/3)

# ws002-p016: cron, crontab, at, and batch

WSID: `ws002`

Phase ID: `p016`

Status: complete baseline

Parent WS: [WS002](https://github.com/awemorris/zedBSD/issues/3)

## Objective and design

Implement one foreground cron service that owns periodic crontabs plus `at`
and `batch` queues, validates ownership and time expressions, runs job command
text through `/bin/sh`, and durably spools output while no mail provider exists.

## Acceptance and result

Periodic and one-shot jobs, credentials, durable queue/output state, recovery,
and installed-system integration reached the WS002 minimum. Shared cases are
indexed in [WS002 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/tests/README.md).

## Interruption record

Not interrupted. Full cron/at portability findings remain WS001 items when
they are POSIX-related.

## Completion conditions

- `cron`, `crontab`, `at`, and `batch` install and operate through the foreground service.
- Ownership, schedule parsing, credentials, durable queue/output, and recovery cases pass.
- Periodic and one-shot jobs execute successfully in the installed QEMU system.

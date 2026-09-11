<!-- awesome-plan project=zedbsd record=ws002-p017 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws002/phase017/phase.md`

親: [ws002](https://github.com/awemorris/zedBSD/issues/3)

# ws002-p017: optional boot-time ntpdate

WSID: `ws002`

Phase ID: `p017`

Status: complete optional feature

Parent WS: [WS002](https://github.com/awemorris/zedBSD/issues/3)

## Objective and design

Provide a bounded, disabled-by-default `/sbin/ntpdate` oneshot that may set the
clock after networking and before cron. Do not add `ntpd`, `adjtime()`, or
`adjfreq()` to this Phase and do not make boot depend on network time.

## Acceptance and result

Success, timeout/degraded boot, ordering, and QEMU integration are part of the
WS002 baseline test plan. The command is a zedBSD extension rather than a POSIX
completion claim.

## Interruption record

Not interrupted. Continuous time synchronization requires a new workstream or
Phase decision.

## Completion conditions

- `/sbin/ntpdate` performs a bounded one-shot clock update when enabled.
- Success, timeout, invalid response, and degraded-boot behavior pass tests.
- The feature remains disabled by default and boot never requires network time.

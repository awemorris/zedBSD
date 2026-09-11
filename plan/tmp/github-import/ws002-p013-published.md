<!-- awesome-plan project=zedbsd record=ws002-p013 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws002/phase013/phase.md`

親: [ws002](https://github.com/awemorris/zedBSD/issues/3)

# ws002-p013: logger and zedBSD syslogd

WSID: `ws002`

Phase ID: `p013`

Status: complete baseline

Parent WS: [WS002](https://github.com/awemorris/zedBSD/issues/3)

## Objective and design

Implement POSIX `logger`, local datagram logging at `/run/log`, foreground
`syslogd`, `/var/log/messages`, and the boot kernel-message snapshot at
`/run/dmesg.boot`. Do not create `/var/log/syslog` or `/var/log/dmesg` in the
initial policy.

## Acceptance and result

Logging startup, messages, kernel snapshot, persistence, and service lifecycle
were included in integrated QEMU acceptance. Shared cases and remaining fault
paths are indexed in [WS002 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/tests/README.md).

## Interruption record

Not interrupted. Broader syslog policy and XSI libc interfaces remain separate
ledger work where applicable.

## Completion conditions

- `logger` and foreground `syslogd` exchange messages through `/run/log`.
- `/var/log/messages` and `/run/dmesg.boot` follow the documented persistence policy.
- Startup, malformed message, unavailable output, restart, and QEMU integration cases pass.

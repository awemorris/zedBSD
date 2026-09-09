# Queue q132: WS019 command staging

Date: 2026-09-09
Status: finished
Authorization: Priority全件のPhase/Queue設計・自走、既存コマンド拡張のユーザー承認。
Timebox: 120 active minutes.
Previous: [q131](queue-q131.md) finished / p016 completed.

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p017](ws019-installation/phase017-command-staging/phase.md) | uncleared | cpの所有権/失敗修正、exclusive staging、Noct argv実行、fresh FAT data/swap作成と検証 |

Noct upstreamへ新規ioctl bindingやprivate helperは追加しない。p004/p005は前提受け入れ後にQueue化する。

Result: [p017 evidence](ws019-installation/phase017-command-staging/results.md). Capacity refusal returned 124 after delayed syscall completion; follow-up FAT correction needed before full staging acceptance.

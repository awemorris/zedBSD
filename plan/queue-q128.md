# Queue q128: WS022 static TLS実装と受け入れ

Date: 2026-09-09
Status: finished
Authorization: Priority全件自走のユーザー明示承認を継続。
Timebox: 各Phase90 active minutesごとに進捗・検証・残件を評価。
Previous: [q127](queue-q127.md) completed。

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws022-p002](ws022-elf-tls/phase002-exec-loader/phase.md) | completed | 確定TCB prefix、PT_TLS検証・新VM初期化、spawn/exec TP設定、i386 GS基盤、loader fixture。 |
| 2 | [ws022-p003](ws022-elf-tls/phase003-thread-runtime-acceptance/phase.md) | completed | static libc/pthread template clone、rtld ABI同時更新、linker TLS PHDR、両x86 guestとdynamic回帰。 |

実装境界は[p001契約](ws022-elf-tls/phase001-contract-and-fixtures/contract.md)。p002のTCB改訂はrtld self初期化も同時に行い、中間build不整合を残さない。p003はp002 gate後に実行。不可達条件はunclearedとして証拠を残す。

結果: [WS022 p003](ws022-elf-tls/phase003-thread-runtime-acceptance/results.md)。全項目completed、WS022完了。

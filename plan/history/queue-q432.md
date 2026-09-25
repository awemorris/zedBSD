<!-- awesome-plan project=zedbsd record=queue-history q432 -->

# Queue q432: host の参照値と guest の 1 check の費用の内訳（ws061-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q432
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「引き続き、expatのconfigureとコンパイルがLinuxと同等水準になることを直近の目標にして、修正と改修を中心に作業を進めてください。」範囲は [ws061-p001](ws061/phase001/phase.md)（測定と内訳。code は変えない）。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q432-i01 | [ws061-p001](ws061/phase001/phase.md) | cleared（host: configure 11 秒・`cc t.c -o t` 83〜92 ms・link 48〜57 ms、guest は 8 倍・7 倍。guest は page fault に律速: 約 30 µs/fault、`clang --version` 5797 fault = 188 ms、`true` 306 fault = 11 ms。kernel の標本: fault 22%、exit の page table の解体 13%、USB の同期 flush 20%。次は p002（fault の固定費用と exit の解体）、p003（fault-around）、ws046-p014） |

依存: なし。

Upcoming Work Outlook: p001 の結果で決める次の Phase（ws046-p014 など）、ws056-p001 の判断（BUG-046）。WS060・BUG-036・039・041・WS055 は fg011 の後。

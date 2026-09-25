<!-- awesome-plan project=zedbsd record=queue-history q425 -->

# Queue q425: VM の commit の課金と上限の現状の調査（ws057-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q425
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「仮想メモリのreserveとcommitを分離して…そういう実装にしたつもりですが、そうなっていなかったら…この機会に修正してほしいです。引き続きパフォーマンス問題の修正と、バグ修正と、上記観点での修正をお願いします。」（design policy 10）。範囲は [ws057-p001](ws057/phase001/phase.md)（調査と設計。code は変えない）。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q425-i01 | [ws057-p001](ws057/phase001/phase.md) | cleared（経路ごとの課金の表と guest の実測。reserve（`PROT_NONE`）と commit は分かれ、commit は上限で ENOMEM、fork も課金。上限は物理 + swap（定義はユーザーの判断待ち）。発見: BUG-048（mmap などの長さが 4 GiB で切り捨て、9 GiB が 1 GiB になる）、`MAP_NORESERVE` 未定義で EOPNOTSUPP） |

依存: なし。

Upcoming Work Outlook: ws058-p001（cache の上限の調査）、ws056-p001 の判断（BUG-046）、ws046-p012、BUG-047（VFS の st_dev）、BUG-036・039・040・041、WS055。

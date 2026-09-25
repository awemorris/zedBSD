<!-- awesome-plan project=zedbsd record=queue-q375 -->

# Queue q375: base の utility の調査と試験の土台（ws043-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: q376
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「/bin/sh の互換性が低いという現状があります。これを徹底的に修正する phase を、優先度を上げて実施したいです。」
WS043 は WS042 の実 script（p005）の前提として 2026-09-24 に作った（guest の sed・grep・awk 等が POSIX の基本の使い方を通さない）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q375-i01 | [ws043-p001](../ws043/phase001/phase.md) | cleared（差分試験の土台、329 件、現状 88/329） |

<!-- awesome-plan project=zedbsd record=queue-q409 -->

# Queue q409: i386（pcat・pc98）の vmunix に LTO（ws053-p004）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「LTOは検討後、実現可能なら、適用をお願いします。」「HALとカーネルを別にコンパイルする必要はないので、統合して大丈夫です。」「引き続き自走してください。」範囲は [ws053-p004](ws053/phase004/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q409-i01 | [ws053-p004](ws053/phase004/phase.md) | cleared（pcat・pc98 も既定で full LTO、両方 QEMU で login。mode の定義を top の Makefile にまとめた） |

依存: ws053-p002（cleared）。人間の判断は要らない。

Upcoming Work Outlook: ws053-p005（規約と最後の回帰。人間の判断は要らない）、WS046 p007（BUG-033、中断中）。

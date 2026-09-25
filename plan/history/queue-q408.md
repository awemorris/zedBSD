<!-- awesome-plan project=zedbsd record=queue-q408 -->

# Queue q408: arm64 の vmunix に LTO（ws053-p003）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「LTOは検討後、実現可能なら、適用をお願いします。」「HALとカーネルを別にコンパイルする必要はないので、統合して大丈夫です。」範囲は [ws053-p003](../ws053/phase003/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q408-i01 | [ws053-p003](../ws053/phase003/phase.md) | cleared（arm64 は既定で full LTO、QEMU raspi4b で boot） |

依存: ws053-p002（cleared）。人間の判断は要らない。

Upcoming Work Outlook: ws053-p004（i386）、p005、WS046 p007（BUG-033、中断中）。

結果: ws053-p003 cleared。arm64（rpi4）の vmunix も既定で full LTO（HAL を含む）。build と check が通り、QEMU raspi4b で login prompt。vmunix +2.0%。実機は未実施。

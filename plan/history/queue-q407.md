<!-- awesome-plan project=zedbsd record=queue-q407 -->

# Queue q407: amd64 の vmunix に LTO を既定で適用（ws053-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「LTOは検討後、実現可能なら、適用をお願いします。これは優先度高めでお願いします。」「HALとカーネルを別にコンパイルする必要はないので、統合して大丈夫です。」範囲は [ws053-p002](../ws053/phase002/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q407-i01 | [ws053-p002](../ws053/phase002/phase.md) | cleared（amd64 は既定で full LTO、回帰は LTO の前と同じ） |

依存: ws053-p001（cleared）。人間の判断は要らない。

Upcoming Work Outlook: ws053-p003（arm64）、p004（i386）、p005、WS046 p007（BUG-033、中断中）。

結果: ws053-p002 cleared。amd64 の vmunix は既定で full LTO（HAL を含む）。`ZEDBSD_KERNEL_LTO`（full・thin・none）で選べ、切り替えると kernel の object が作り直される。
回帰: boot test、guest の sh の差分試験 1388/1425（LTO の前と同じ数）、make 91/91、対話 41/41。

<!-- awesome-plan project=zedbsd record=queue-q389 -->

# Queue q389: libc の浮動小数の十進変換と stdout の buffer（ws043-p012）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「続けてください。自走をお願いします。」と WS043 の計画（p011 の受け入れに要る依存）。範囲は [ws043-p012](../ws043/phase012/phase.md)。HAL の変更は無い。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q389-i01 | [ws043-p012](../ws043/phase012/phase.md) | cleared（glibc と 1073462 件で違い 0、guest で p011 の 3 件が通る、sh の guest 試験に libc による差なし、両 arch の boot test） |

依存: なし（libc）。人間の判断は要らない。

中断（2026-09-24）: 実行中にユーザーから RPi4 実機が起動しないとの報告があり、先に [ws044-p006](../ws044/phase006/phase.md)（framebuffer の data cache、HAL の差分を承認）を行った。p012 はその後に再開する。

Upcoming Work Outlook: ws043-p011 の再開、ws043-p013（file の utility）で WS043 を終える。その後 WS042 p005・p006、WS046、WS047 p001。

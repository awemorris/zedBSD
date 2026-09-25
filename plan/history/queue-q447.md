<!-- awesome-plan project=zedbsd record=queue-history q447 -->

# Queue q447: `cc t.c -o t` を host と同等以上に（ws061-p009、再開）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q447
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「ccの実行時間もホストと同等以上に速くなることを目標にして、改修を進めてください。」（q445 で中断した範囲の再開）。2026-09-26 ユーザー「system call の入口を … 優先で … 規約適合は最後でいいです。続けてください。」の順で p010 の後。範囲は [ws061-p009](../ws061/phase009/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q447-i01 | [ws061-p009](../ws061/phase009/phase.md) | cleared（loader の探索の hash を 1 度に、予約の名前の判定、再配置の segment の検査の cache、`cc`・`ld`・`clang++` を link に、`spin_trylock` の TTAS。`cc` 90〜99 → 75〜85 ms（host 83〜85）、configure 9.1〜9.4 秒（host 10.7）。BUG-052 に inode の共通の pool の限界を追記、F-017） |

Upcoming Work Outlook: WS064（make の `-j`）、規約の適合（最後）。

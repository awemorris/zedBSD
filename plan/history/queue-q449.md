<!-- awesome-plan project=zedbsd record=queue-history q449 -->

# Queue q449: 並列の make の性能（ws064-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q449
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「makeは-jに対応させて、並列makeの実行時間もホストと同等以上にしてください。」、途中の指示「fork系にこだわらず、posix_spawnなども検討してください」「vforkもUAPIで公開してあげてください」「目標達成まで続けてください」。範囲は [ws064-p002](../ws064/phase002/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q449-i01 | [ws064-p002](../ws064/phase002/phase.md) | cleared（`make -j4` 7.3〜7.8 → 4.94〜5.00 秒、host 5.14〜5.18 秒。mutex の回転と速い道、`vfork` の system call と libc の `vfork()`・vfork の posix_spawn、make と sh の posix_spawn、fork の一括、destroy の解放を lock の外へ、逆写像の O(1)、image の `/usr/lib/libc.so` を今の build のものに） |

Upcoming Work Outlook: ws064-p004（sh の process の生成、ユーザー指示）、規約（ws061-p011・ws064-p003・ws063-p002、最後）。

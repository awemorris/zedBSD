<!-- awesome-plan project=zedbsd record=queue-history q428 -->

# Queue q428: cache の上限を現代の機械向けに変える（ws058-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q428
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「…キャッシュサイズが極端に小さくなっています。…メモリ使用量はあまり気にしなくていいです。4GBのメインメモリや、16GBくらいのスワップファイルサイズを前提にしてOKです。」（design policy 10）。範囲は [ws058-p002](ws058/phase002/phase.md)（p001 の設計 1〜5）。HAL は変えない。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q428-i01 | [ws058-p002](ws058/phase002/phase.md) | cleared（buffer cache 物理/8・hash 2^16、page cache の target 物理/2、VM object cache 256、snapshot 1024、I/O pool 64 MiB、file 表 2048、inode cache 2048/512、overlay の表 4096。最初の値（file・inode 8192、object 1024）は overlay の固定の表と「cache の object が file の表を使う」ことで ENOSPC/ENFILE の退行になり、bisect で原因を特定して有界に。回帰: boot PASS、8 GiB と 512 MiB で make 91/91、sh 1388/1425（同じ集合）、SMP 0、configure 96 → 89 秒、`cc` 1.0 → 0.63 秒） |

依存: ws058-p001（cleared）。

Upcoming Work Outlook: ws057-p003（裏打ちの定義の判断待ち）、ws056-p001 の判断（BUG-046）、ws046-p012、BUG-047、BUG-036・039・040・041、WS055。

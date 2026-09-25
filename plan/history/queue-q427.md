<!-- awesome-plan project=zedbsd record=queue-history q427 -->

# Queue q427: cache の上限の定数と根拠の調査、新しい上限の設計（ws058-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q427
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「…キャッシュサイズが極端に小さくなっています。現在は現代のコンピュータをターゲットにしているので、メモリ使用量はあまり気にしなくていいです。4GBのメインメモリや、16GBくらいのスワップファイルサイズを前提にしてOKです。」（design policy 10）。範囲は [ws058-p001](ws058/phase001/phase.md)（調査と設計。code は変えない）。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q427-i01 | [ws058-p001](ws058/phase001/phase.md) | cleared（cache と表の上限の一覧。比例のもの（buffer 物理/16、page cache 物理/4）と固定で小さいもの（VM object 32、file 表 192、inode 512、I/O pool 4 MiB）を分け、新しい上限と 512 MiB の下限を設計） |

依存: なし。

Upcoming Work Outlook: ws058-p002（上限の変更と回帰）、ws057-p003（裏打ちの定義の判断待ち）、ws056-p001 の判断（BUG-046）、ws046-p012、BUG-047、BUG-036・039・040・041、WS055。

<!-- awesome-plan project=zedbsd record=queue-q415 -->

# Queue q415: UFS の directory を複数 block に、journal の経路（ws054-p003）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー「すべて承認します。」（WS054 の実装 p002〜p004）、「それまで可能な限り自走してください。」範囲は [ws054-p003](ws054/phase003/phase.md)。
Timebox: ユーザーの朝 9 時頃まで。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q415-i01 | [ws054-p003](ws054/phase003/phase.md) | cleared（journal の volume でも 12 block まで育つ。電源断 3 回の replay の後も一貫） |

依存: ws054-p002（cleared）。人間の判断は要らない。

Upcoming Work Outlook: ws054-p004（guest の試験・coreutils・回帰）、ws046-p008 の再開、WS055。

## 結果の要約（ws054-p003）

- 最初の block を作る journal の group を「次の block を足す」group に一般化、名前を入れる group（作成・link・rename）が名前の block を選ぶ、rename は古い名前と新しい名前の block を探す。
- guest の journal の volume: 作成・削除・rename・rmdir・上限 4031（12 block）で ENOSPC、再起動の後も同じ、12 block の `rm -r` と 3 block の directory の `mv`。電源断 3 回の後の replay で名前が途切れず、host の検査で一貫。
- journal の volume の名前の操作が 1 回約 100 ms、続けると guest が応答しない（BUG-040）。

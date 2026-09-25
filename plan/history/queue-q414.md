<!-- awesome-plan project=zedbsd record=queue-q414 -->

# Queue q414: UFS の directory を複数 block に、journal 無しの経路（ws054-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー「すべて承認します。」（WS054 の実装 p002〜p004）、「それまで可能な限り自走してください。確認事項は記録して後回しにしてください。」範囲は [ws054-p002](ws054/phase002/phase.md)。
Timebox: ユーザーの朝 9 時頃まで。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q414-i01 | [ws054-p002](ws054/phase002/phase.md) | cleared（journal 無しの経路で directory が 12 block まで育つ。guest の試験と host の fsck 相当の検査、coreutils の展開） |

依存: ws054-p001（cleared）。人間の判断は要らない。

Upcoming Work Outlook: ws054-p003（journal の経路）、ws054-p004（guest の試験・coreutils・回帰）、ws046-p008 の再開、WS055。

## 結果の要約（ws054-p002）

- `ufs.c` の directory の探索・追加・削除・置き換えを全ての block に。追加は空き → 最後の block の chunk → 新しい block（直接の 12 block まで、ENOSPC）。journal の mount は最初の block だけ（p003 まで）。
- guest: 長い名前 1500・短い名前 3000・rename・rmdir・上限 4031（12 block）で ENOSPC、再起動の後も同じ。host の fsck 相当の検査で一貫。coreutils の source の展開が通った。
- host の UFS 試験の土台が build できない（BUG-039）。作業の disk を usb-storage にすると usb-net が落ちることがあり（BUG-036 に追記）、NVMe に替えた。`ARG_MAX` 16384（F-011）。

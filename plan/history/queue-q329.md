<!-- awesome-plan project=zedbsd record=queue -->

# Queue q329: zlib と expat の package（ws034-p021）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-23）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q328 finished（履歴 `plan/history/queue-q328.md`）
<!-- awesome-plan-current:end -->

Approval: ユーザーの夜間の自律実行の指示（2026-09-23、`plan/master.md`「夜間の自律実行」）。
版・入手元・検証値は ws034-p001 の `plan/ws034/package-inventory.md` による。
Start UTC: 2026-09-23T13:40:00+00:00

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q329-i01 | [ws034-p021](../ws034/phase021/phase.md) | cleared | `userland/packages/libs/zlib`・`libs/expat`。CMake での cross build、symbol versioning なし、版付き SONAME と symlink の image 投入 |

## 範囲外

HALの変更。aggregate `make check`。commitは `git commit -m WIP` のみでpushしない。

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q329-i01 | ws034-p021 | **cleared**。zlib 1.3.2・expat 2.8.5 の package。SONAME の実体＋symlink を `/usr/lib` に。ゲストで round-trip（crc32 が host と一致）と `xmlwf` |

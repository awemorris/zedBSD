<!-- awesome-plan project=zedbsd record=queue-q413 -->

# Queue q413: UFS の複数 block の directory の調査と設計（ws054-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「引き続き自走してください。」WS046（ユーザーが優先度を上げた実 package の build）の coreutils が待つ BUG-038 の設計。範囲は [ws054-p001](ws054/phase001/phase.md)（設計のみ、code は変えない）。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q413-i01 | [ws054-p001](ws054/phase001/phase.md) | cleared（設計: 直接の 12 block まで、journal は block を足す group を先に。実装は p002〜p004） |

依存: なし。人間の判断は要らない（設計のみ。WS054 の優先度はユーザーの確認を待つが、設計は WS046 の依存として進める）。

Upcoming Work Outlook: ws054-p002（journal 無しの経路。WS054 の優先度はユーザーの確認を待つ）、ws046-p009（BUG-033 の残り）、ws046-p005（規約と回帰）。

## 結果の要約（ws054-p001）

- 読む側（`next_dirent()`、lookup・readdir）は複数 block を既に読める。書く側（`dir_find_record`・`dir_add`・`dir_remove`・`dir_replace`）と journal の group（creation・link・rename・remove、最初の block を作る group）が `direct[0]` だけを扱う。
- 設計: 直接の 12 block まで（96 KiB、約 3000 の entry、F-010 で間接の block）、chunk の形は今のまま、縮めない、journal は「block を足す」group を先に単独で commit。HAL の変更は要らない。
- UFS の host 試験の土台（`plan/ws001/tests/directory-fsync-host-test.mk`）は `io-stats.c` の統合で build できない。p002 で直す。
- 実装: p002（journal 無し）、p003（journal）、p004（guest の試験・coreutils・規約と回帰）。

<!-- awesome-plan project=zedbsd record=queue-q400 -->

# Queue q400: GNU 互換の make の調査と設計（ws046-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「自走を続けてください。」と WS046 の計画（2026-09-24 ユーザー指示「GNU互換のmakeも必要です。…autotoolsの出力を実行できる程度には」）。範囲は [ws046-p001](../ws046/phase001/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q400-i01 | [ws046-p001](../ws046/phase001/phase.md) | cleared（機能の一覧と設計、差分試験 91 件が GNU make で通る） |

依存: なし（WS042・WS043 は完了）。人間の判断は要らない（調査と設計。範囲外は WS046 に書かれている）。

Upcoming Work Outlook: ws046-p002（POSIX make の核）、p003（GNU の機能）、WS047 p001、WS045、WS048（後回し）。

結果: ws046-p001 cleared。expat・zlib・coreutils の生成された Makefile を数え、automake の出力は POSIX make（2024 版）と入れ子の変数参照と
いくつかの GNU の特別な target で動くと分かった（GNU の関数と条件は coreutils の maintainer 用 `GNUmakefile` だけ）。`userland/base/make` の設計
（9 file、`GNUmakefile` を読まない、`-j` は無視、GNU make と名乗らない、message と内蔵の rule は GNU の形）と、差分試験 `plan/ws046/tests/make-diff.py`
（91 件、GNU make で全て通る）を作った。範囲外は Future Work F-007。

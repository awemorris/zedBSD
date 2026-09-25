<!-- awesome-plan project=zedbsd record=queue-q395 -->

# Queue q395: vi mode の行編集（ws042-p008）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「自走を続けてください。」と WS042 の計画。範囲は [ws042-p008](../ws042/phase008/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q395-i01 | [ws042-p008](../ws042/phase008/phase.md) | cleared（vi mode。guest の serial console で 41/41、host の差分試験 1417/1425 のまま） |

依存: ws042-p007（cleared）。人間の判断は要らない（vi mode の範囲は XCU の定義と計画の範囲）。

Upcoming Work Outlook: ws042-p009（WS042 の規約と回帰。readline.c・options.c・input.c の既存の違反を含む）、WS046（GNU make）、WS047 p001、WS048（RPi4 の USB、後回し）。

結果: ws042-p008 cleared。vi mode の行編集（`readline.c` の vi の editor、`rl_editing_mode`、sh の `set -o vi`/`emacs`）。
検証: guest の serial console の対話試験 41/41、host の vi の試験 26/26、host の sh の差分試験 1417/1425、amd64・aarch64 の image の build（warning 0）、
boot test（amd64・RPi4 の QEMU）で login prompt。実機は未実施。
記録: [BUG-030](../bugs/BUG-030.md)（amd64 の起動時の USB storage の error 42、間欠、この変更と無関係）、[Future Work F-006](../future-work.md)（vi の残りの命令）。

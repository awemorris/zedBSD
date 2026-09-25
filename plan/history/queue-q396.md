<!-- awesome-plan project=zedbsd record=queue-q396 -->

# Queue q396: 規約の全文照合 libedit（ws042-p009）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「自走を続けてください。」と WS042 の計画（p009 を p009〜p012 に分けた）。範囲は [ws042-p009](../ws042/phase009/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q396-i01 | [ws042-p009](../ws042/phase009/phase.md) | cleared（libedit の規約の違反 0。端末への出力は書き直す前と同じ） |

依存: ws042-p008（cleared）。人間の判断は要らない（意味を変えない規約の書き直し）。

Upcoming Work Outlook: ws042-p010（sh の規約）、p011（ls の規約）、p012（最後の回帰と WS042 の完了）、WS046（GNU make）、WS047 p001、WS048（後回し）。

結果: ws042-p009 cleared。`userland/base/libedit` を規約の全文に合わせた（style-check.py の違反 0、禁止の comment の形 0）。
`readline()` を分け、emacs mode の処理を `emacs_key()` に出して vi mode と同じ形にした。書き直す前と後で端末への出力の byte 列が同じ。
検証: host の行編集の試験 33/33、host の sh の差分試験 1417/1425、guest の対話 41/41、amd64・aarch64 の build（warning 0）、amd64 の boot test。
記録: [BUG-031](../bugs/BUG-031.md)（起動時の console の行の混ざり、無関係）。

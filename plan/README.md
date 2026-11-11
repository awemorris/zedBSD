# zedBSD の計画（plan/）

Awesome Plan の記録。毎回守る規則はリポジトリ直下の [AGENTS.md](../AGENTS.md)（短く保ち、詳細はここの file に置く）。どれを読むかは AGENTS.md §1。

| file | 中身 | いつ読むか |
| --- | --- | --- |
| [master.md](master.md) | 今の状態（担当・Queue・Focus・優先順位・未決・次の番号）、目的・Milestone | 毎回の始め |
| [q1.md](q1.md) | Q1 の運用（ユーザーとのやり取り、問いの振り分け、cockpit の書き方、担当の割り当て、不在の間の進め方） | Q1 は毎回 |
| [cockpit.md](cockpit.md) | ユーザーが監視する一枚（担当・WS の進捗・判断を待つ事・拾える試験・決まった事）。Q1 が保つ写し | Q1 は毎回 |
| [workstreams-open.md](workstreams-open.md) | 未完の WS の一覧（ws.md の写し） | WS を選ぶ・状態を直す時 |
| [queue.md](queue.md) | 今の Queue（承認・範囲・担当・結果） | Q1 は毎回、担当は自分の行 |
| [agents/protocol.md](agents/protocol.md)・[agents/registry.md](agents/registry.md) | 担当の運用、担当の台帳 | 担当の起動・Q1 の割当の時 |
| `wsNNN/ws.md`・`wsNNN/phaseNNN/phase.md` | WS と Phase（決定の原文・再開点・結果） | 担当の Phase の時 |
| [guardrail.md](guardrail.md) | 範囲・HAL・出典の規則・承認済みの HAL 差分 | source を変える前に該当の節 |
| [coding-style.md](coding-style.md) | C の規約（全文が正本） | C を書く前に |
| [master-design-policy.md](master-design-policy.md) | 設計方針、全体に効く決定（§11）、対象 platform | 設計の前に |
| [tools.md](tools.md) | 道具・回帰の試験・実機・判定の方法の例外・QEMU の解析・環境の作り直し | 試験の前に |
| [amaterrace.md](amaterrace.md) | Amaterrace GPU・GHCI の仕様との仕事の仕方（Claude は監査、Gemini が RTL） | GHCI・WS347 の時 |
| [makefile-style.md](makefile-style.md) | Makefile・`mk/` の規約（変更はユーザーに示してから） | Makefile を書く前に |
| [tests.md](tests.md) | シナリオ試験（AAT、`tests/scenarios/`） | AAT を使う時 |
| [known-bugs.md](known-bugs.md)・`bugs/` | Bug Board と ticket | Bug の時 |
| [future-work.md](future-work.md) | 将来の案（実行の許可ではない） | 計画の時 |
| [workstreams-closed.md](workstreams-closed.md) | 完了・取り下げた WS の一覧 | 参照の時だけ |
| [record-integrity.md](record-integrity.md) | 記録の整合を保つ手順 | ラップアップの前 |
| [config.md](config.md)・`records.json`・`milestones.json` | Awesome Plan の設定、GitHub の Issue の対応 | GitHub の同期の時 |
| [history/](history/index.md) | 終わった Queue・Past Log・古い記録（指示ではない） | 経緯を調べる時だけ |
| `standards/` | 個別の標準（browser component など、guardrail から link） | guardrail の指示の時 |

- Phase に固有の試験・一時物は `plan/wsNNN/tests/`・`plan/wsNNN/temp/`（temp は git に入れない）。シナリオ試験（AAT）は [tests.md](tests.md)・`tests/scenarios/`。
- Bug は `bugs/BUG-NNN.md`（本体）と [known-bugs.md](known-bugs.md)（Bug Board、写し）。担当ごとの lane は `agents/P<n>/queue.md`。

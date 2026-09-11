> Superseded: Awesome PlanのGitHub運用を開始。現行は[config](../config.md)と[移行状態](../migration-status.md)。以下は旧提案の履歴。

# plan/ 整理・Awesome Planローカル移行案

日付: 2026-09-10
状態: レビュー用。一時資料の退避・本計画の作成と、WS/Phaseディレクトリの番号名への変更は実施済み。
新規の開発目標・WS・Phase・実行Queueは作成しない。

## 方針

- local-onlyで移行する。GitHub Issues/Projectsへの書込みや同期基盤は対象外。
- 既存WS/Phase/Queue/BUGのIDを維持する。ディレクトリはユーザー指定の
  `wsXXX/phaseYYY/`へ統一し、名前の接尾辞を付けない。
- 現行状態と過去の成果を分離し、旧完了記録を現行実装の証拠に読み替えない。
- 全履歴の書き直しや全WSの再審査はせず、現行索引と残作業から整える。
- スキルは導入時にレビューしたcommitを固定し、MITライセンスとともに保存する。
  参照元: https://github.com/awemorris/AwesomePlan/blob/main/awesome-plan.md

## 今回の整理（実施済み）

`plan/old/`へ、buffer設計/レビュー4件、FS資料4件、WiFiレビュー、
リファクタ報告、review1とバックアップ、coding-styleのバックアップ、旧masterを
移動した。本文の判断・結果は保存し、相対リンクと明示的なplanパスを追従した。
現行coding-style、設計方針、運用規則は直下に残す。

`queue-qNNN.md`と`agent2-queueNNN.md`は一時メモではなく実行履歴なので、
次段のhistory整理で扱う。削除・番号変更・結果の書換えはしない。

## 移行後の構成案

```text
plan/
  README.md, AGENTS.md
  config.md, master.md, queue.md, guardrail.md
  future-work.md, known-bugs.md
  coding-style.md, master-design-policy.md
  standards/automation.md             # 自動検査と目視確認の範囲
  standards/concise/                  # 必要な範囲だけ作る
  bugs/                              # 必要な詳細票から分離
  history/index.md, queue-qNNN.md, agent2-queueNNN.md
  wsXXX/ws.md
  wsXXX/phaseYYY/phase.md
  wsXXX/tests/, temp/
  old/                               # 旧提案・一時レビュー
  tmp/                               # 移行案などの作業資料
```

スキル本体は`docs/agent/awesome-plan.md`へ置く案。
`config.md`が実際の場所と既存ID形式を定義する。既知バグ索引の名前は
`known-bugs.md`を再利用し、規定例の名前へ合わせるためだけに移動しない。

## 手順

| 段階 | 作業 | 終了の目安 |
| --- | --- | --- |
| 1. 運用入口 | スキル固定版、config、Guardrailを作り、AGENTSから参照。governance/MWPの重複規則を整理 | 入口が一つに定まり、旧文書を現行指示と誤読しない |
| 2. 現行索引 | masterの既存目的・優先度からMilestoneとFocused Goalの対応案を作る。Futureを分離 | 各WSのPrimary Milestoneが明確。不明な意図はdraftのまま |
| 3. 残作業の状態 | WS025/WS009を優先して状態・依存・再開条件を正規化 | 現行状態を履歴の長文から探さず読める |
| 4. 履歴・バグ | Queue履歴をhistoryへ移し索引化。Bug詳細は長いもの/現役のものから分離 | 旧IDで追跡でき、元の証拠へ到達できる |
| 5. 軽い整合確認 | リンク、ID、親、状態、取消し、承認の扱いを確認 | 移行結果と残る判断事項を報告できる |

各段階は小さな文書差分として進める。新しい開発目標を作らず、マイルストーンは
既存の目的を整理したものに限定する。意図が確定しない対応関係を推測で承認済みにしない。

## 状態と既存判断の移行

- Phaseの旧completedは、証拠に沿ってclearedへ対応付ける。過去Queue本文は
  当時の表記を保存し、索引に旧状態の読み方を記載する。
- p028は最後の実行状態unclearedとdisposition=canceledを分離する。
- p029は修正後の確認待ち、p030は実装確認済み/動作・比較確認残。
- p032は現行対応未着手。旧試行は削除せず残し、未着手と履歴を区別する。
- WS025はincomplete。WS009はDOC-54/WS014待ち。他の受け入れ済みWSを
  移行だけを理由に再開しない。
- q303はfinished/unclearedの停止履歴。移行でactiveへ戻さない。
- 自走の過去承認と、その後の停止・取消しを時系列で整理する。移行はコード実行の
  再開許可ではない。Outlookにも実行許可を持たせない。

## Guardrailで整理すること

- HALの責務分担・hal.h変更制限と、67b28ce0で置換された旧承認を区別する。
  古いVM移設案やpmem引数展開案を再び指示にしない。
- coding-style.mdを正本にする。簡潔版は必要な範囲から作り、機械検査で
  確認できないルールも明記する。ソースの一括整形はしない。
- 無コミット、aggregate make check禁止、.internal非参照、RTL8822Bの
  ライセンス分離.inc維持など、既存の個別指示を出典付きで集約する。
- 新スキルの規約確認Phaseを、完了済みWSへ機械的に追加しない。
  明示的な過去受け入れを移行上の扱いとして記録し、残るコード作業には
  既存の終盤Phaseへ確認条件を組み込めるか検討する。

## 確認範囲と対象外

移動対象への参照、索引からの到達、重複ID、親WS、Primary Milestone、取消しと
完了の分離を簡単に確認する。欠けている過去承認・実験条件を捏造しない。
既存の壊れたリンクは移行で生じたものと分け、全面修復を完了条件にしない。
ソース修正、OSビルド/受け入れ再実行、GitHub同期、コミット作成は対象外。

## ディレクトリ改名の実績

ユーザー指示によりWS 26件・Phase 335件を番号名へ変更。関連する文書・
ビルド/テスト参照も追従した。旧新対応は
[改名一覧](ws-phase-directory-renames.json)に保持。実験ログ本体は改変せず、
内部に残る旧絶対パスはこの一覧で解釈する。Awesome Plan本体の導入は未実施。

## Issue本文の表示方針（ユーザー指定）

Master/WS本文の先頭に「登録済みの子Issue」のリンク羅列を追加しない。
GitHubの参照表示を利用し、本文の目的・状態・計画表を読みやすく保つ。
この指定は既存の計画表やPhase側の親リンクを削除するものではない。

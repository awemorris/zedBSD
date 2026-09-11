# Awesome Plan adoption / handoff

2026-09-11。運用モードはgithub。次セッションの入口はリポジトリ直下AGENTS.md。

## 導入済み

- 上流314a669f57265da3084ffac810871b0e660c9526の仕様とMITライセンスを固定保存。
  上流にSKILL.mdはなく、公式に記載されたAGENTS.md参照方式でロードする。
  グローバルな個人スキル設定には依存しない。
- config / Guardrail / 全文規約・自動検査範囲 / 起動手順を整備。
- 309件のQueue履歴をhistory/へ、旧MWP-Q/運用指示をold/へ移動。
  ID・結果を維持し、現行文書のリンクを追従。tmpの送信済みスナップショットは履歴。
- Master/361既存Issueを再利用し、5常設Boardを追加。共有対応表records.json。
  plan/.sync/は366件のローカル・リモート基準を別々に保持する。
- Queue #362、Guardrail #363、Future Work #364、Bug Board #365、Past Log #366。
- Project #2へ常設Boardを登録。Bugsビューは既存台帳の入口。
- MG001〜MG009をnative Milestone化。360件のWS/Phaseの親Issueと
  Primary Milestone対応を全件読戻し確認。既存の分類フィールドも保持。
- body送信のjournal、版比較、衝突保存、read-back、再送時の重複防止を実装。
  バックグラウンドサービスはない。コメント/構造/Projectの同期はagent-driven。
- 開発Queueを新規作成・開始していない。q303はfinished/stoppedのまま。

## 引き継ぐ移行残件（開発の再開許可ではない）

| 項目 | 扱い |
| --- | --- |
| 旧Issueのopen/closed | 旧取込時は全件open。2026-09-11にユーザー指示でWS025/WS019/WS006/WS022/WS002とWS025-p029/p030/p032/p038を閉鎖し読み戻し確認。その他の旧Issueには計画上のcleared/completedとの差が残る。対象を次に扱う際に本文の範囲・証拠とユーザー受け入れを確認し、必要なコメント/closeを同期。単純な一括Done化はしない。 |
| 5 Phaseの旧状態 | ws003-p017/p025、ws004-p028/p029/p030は条件付き完了/置換の解釈が必要。Project状態未設定、本文とStatus Noteを保持。 |
| 旧Bug詳細 | 既存BUG-001〜022はBug Board本文の台帳で保持。次の調査・状態変更時に必要なBug Ticketへ分離して相互リンク。新規Bugは最初からTicketと索引を作る。 |
| 旧Queue Issue化 | 履歴はファイル索引に残る。次の新規Queue以降はexact approval/attempt/evidenceを独立履歴Issueにも保存。過去承認を捏造しない。 |
| native dependencies | 過去の前提条件・出力依存は計画本文が正。次のQueue詳細化で必要な範囲を実装と照合し、対応可能なnative依存を同期。 |
| 規約確認 | 全文を利用。簡潔版未作成。完了済みWSを移行だけで再開しない。残るコード作業の詳細化時に既存終盤Phaseへの確認条件を整える。 |
| 公開資料 | 本コミットは作成/公開していない。新規ローカル規約・証拠は未公開と明記。ユーザーのpush許可を推定しない。 |
| 自動同期 | Issue body以外はスキルの手順に沿うgh+outbox運用。意味的な自動マージ、原子的CAS、バックグラウンド同期は未実装。 |

GitHubの共有計画とローカルの詳細証拠を並行する移行状態であり、過去すべての
ライフサイクル・履歴・検証を遡及的に完全移行したとは主張しない。
これらの残件は新しいWSや実行Queueを勝手に生成する理由にはしない。

## 確認

`python3 -m unittest discover -s plan/tools -p test_sync.py`: 6 checks PASS。
同期基準/衝突保護/タイムアウト後の再送/準備後のローカル変更とIssueのcancel/close変更を確認。
コード/OSの再ビルドや受け入れ試験は今回の対象外。

最終読戻し: 366件のIssueとキャッシュ基準が一致、360件のnative親子/Milestone
対応を確認。未送信操作なし。詳細は[検証記録](history/awesome-plan-adoption-verified.json)。
移行中のGitHub 504は実状態確認後に未反映分だけ再送し、全件照合した。

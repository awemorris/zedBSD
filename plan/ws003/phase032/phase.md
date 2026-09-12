<!-- awesome-plan project=zedbsd record=ws003-p032 -->

<!-- awesome-plan-current:start -->

Status: planning
Phase disposition: canceled
Lifecycle: closed
Current Focus: No

2026-09-12ユーザー指示で本Phaseを終了。未完了事項は[Future Work F-004](https://github.com/awemorris/zedBSD/issues/364)へ保留として移管。試験PASSやclearedへ変更しない。再選択時は新WSを作る。

<!-- awesome-plan-current:end -->

<details>
<summary>終了前の計画・試行履歴（現在の再実行指示ではない）</summary>

# ws003-p032: 実機インストーラbring-up変更の最終規約確認

Phase ID: `ws003-p032`
Status: planning
Phase disposition: normal
Date: 2026-09-11
Decision source: current user, this task (planning only).

## ゴール・対象

2026-09-11の4機種インストーラbring-upで実際に変更したsource、build/config、loader、関連ドキュメントと試験を、最終成果物に対して規約・所有権・検証整合の観点から確認する。
既存WS003/WS019/WS025の受け入れ済み成果を、移行だけの理由で再開・再試験しない。現在のbring-up変更一覧を基準に対象を確定し、既存の適用規則・明示的な例外を記録する。

## 手順・完了条件

近接コードを規約の代用品にせず `plan/coding-style.md` の全文該当規則と14節checklistを確認する。Cの配置・宣言・評価順・所有権・コメント、assemblyのABI/配置、Make/Pythonの設定と成果物依存、ライセンス境界を確認する。変更Cファイルには既存clang-format設定によるdry-runを適用し、対象diffの空白確認と各Phaseのfocused check/build結果を照合する。
適用ファイル/規則、版、例外、実行コマンド、結果、未実施を記録する。規約未解決や最終source後の未確認変更があればclearedにしない。確認後にコードが変わった場合は影響範囲を再確認する。
機種別の実機受け入れをこの規約確認で代用しない。最終機種受け入れと本Phaseの結果を合わせてfg004の成果を判断する。対象変更が出揃う終盤に有限Queueを選ぶ。

## 共通の制約・実行境界

親: [ws003](../ws.md)。Primary Milestone: MG003。今回の指示は計画更新。Queue: none / 実装未承認。
[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則、`plan/standards/automation.md`に従う。非CのMake/Python/assemblyは現行規約とABI・配置・サイズ制約を守る。規約のローカル資料を未公開URLとして捏造しない。
`include/hal/hal.h`・HAL責務は別の明示指示なしに変更しない。RTL8822Bのライセンス分離を保持。既存変更を保持し、aggregate `make check`、commit、pushは行わない。
実装前に有限Queueの範囲・時間枠を決める。検証は変更箇所に対応するfocused checkと選択構成の `make -j16`。PC98は維持対象qemu-pc98、amd64はqemu-system-x86_64を使い、共有build/runtimeは直列。試験用媒体は使い捨て。実機書込み対象・起動方式は具体化してから扱う。
結果にはsource/config・image hash、コマンド、観測、未実施項目、残課題を記録。旧QEMU/実機証拠を現在の成果物の合格へ読み替えない。実機成功をQEMU成功で代用しない。

</details>

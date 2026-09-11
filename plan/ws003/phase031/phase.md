# ws003-p031: Let's Note LX6 インストール実機受け入れ

Phase ID: `ws003-p031`
Status: planning
Phase disposition: normal
Date: 2026-09-11
Decision source: current user, this task (planning only).

## ゴール

Let's Note LX6でインストールが行える。

## 前提と手順

[ws003-p028](../phase028/phase.md)のUSB起動・bootパーティション識別・init到達を先行確認する。既存[ws019](../../ws019/ws.md)の対応済みインストール経路を使い、対象媒体・既存データ・text/graphicモード・FAT/native方式を具体化する。
正しい対象に通常インストールを行い、インストール先からloader→kernel→root→init/login・基本操作を確認する。USB起動の回復だけで機種全体のインストール完了にはしない。

## 完了条件・未決入力

同一通常成果物について、USB起動修復とインストール・インストール先起動の結果を区別して記録する。source/target識別子、hash、設定、実際の画面/ログ、残条件を保存する。
LX6の起動モード・対象ディスク・インストール方式は未確定。PC98/Latitude/SV7の追加試験をこのPhaseの前提にしない。

## 共通の制約・実行境界

親: [ws003](../ws.md)。Primary Milestone: MG003。今回の指示は計画更新。Queue: none / 実装未承認。
[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則、`plan/standards/automation.md`に従う。非CのMake/Python/assemblyは現行規約とABI・配置・サイズ制約を守る。規約のローカル資料を未公開URLとして捏造しない。
`include/hal/hal.h`・HAL責務は別の明示指示なしに変更しない。RTL8822Bのライセンス分離を保持。既存変更を保持し、aggregate `make check`、commit、pushは行わない。
実装前に有限Queueの範囲・時間枠を決める。検証は変更箇所に対応するfocused checkと選択構成の `make -j16`。PC98は維持対象qemu-pc98、amd64はqemu-system-x86_64を使い、共有build/runtimeは直列。試験用媒体は使い捨て。実機書込み対象・起動方式は具体化してから扱う。
結果にはsource/config・image hash、コマンド、観測、未実施項目、残課題を記録。旧QEMU/実機証拠を現在の成果物の合格へ読み替えない。実機成功をQEMU成功で代用しない。

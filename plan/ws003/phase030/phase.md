# ws003-p030: Let's Note SV7 インストール実機受け入れ

Phase ID: `ws003-p030`
Status: planning
Phase disposition: normal
Date: 2026-09-11
Decision source: current user, this task (planning only).

## ゴール

Let's Note SV7でインストールが行える。今回新しい故障症状は報告されていない。

## 前提と手順

[ws003-p020](../phase020/phase.md)/[ws003-p021](../phase021/phase.md)の過去のUSB起動成功を参考に、現行成果物でインストーラを起動できることを確認する。既存[ws019](../../ws019/ws.md)の対応済みインストール経路を用い、対象媒体・既存データ・text/graphicモード・FAT/native方式を具体化する。
通常インストールとインストール先からのloader→kernel→root→init/login・基本操作を確認する。新しい停止があれば最初の失敗段階と成果物を保存し、このPhase内の有限な調査・修正範囲を具体化する。症状のない段階でACPIやUSBを再設計しない。

## 完了条件・依存

SV7の現行通常成果物に対応するインストール結果とインストール先起動を、hash・設定・画面/ログ付きで受け入れる。過去のUSBログイン成功は今回のインストール成功ではない。
PC98/LX6の修復と依存しない。対象ディスク・起動方式・インストール方式は未確定。実機操作が必要になる前に具体化する。

## 共通の制約・実行境界

親: [ws003](../ws.md)。Primary Milestone: MG003。今回の指示は計画更新。Queue: none / 実装未承認。
[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則、`plan/standards/automation.md`に従う。非CのMake/Python/assemblyは現行規約とABI・配置・サイズ制約を守る。規約のローカル資料を未公開URLとして捏造しない。
`include/hal/hal.h`・HAL責務は別の明示指示なしに変更しない。RTL8822Bのライセンス分離を保持。既存変更を保持し、aggregate `make check`、commit、pushは行わない。
実装前に有限Queueの範囲・時間枠を決める。検証は変更箇所に対応するfocused checkと選択構成の `make -j16`。PC98は維持対象qemu-pc98、amd64はqemu-system-x86_64を使い、共有build/runtimeは直列。試験用媒体は使い捨て。実機書込み対象・起動方式は具体化してから扱う。
結果にはsource/config・image hash、コマンド、観測、未実施項目、残課題を記録。旧QEMU/実機証拠を現在の成果物の合格へ読み替えない。実機成功をQEMU成功で代用しない。

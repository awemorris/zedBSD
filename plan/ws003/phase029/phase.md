# ws003-p029: PC-9821V13 インストール実機受け入れ

Phase ID: `ws003-p029`
Status: planning
Phase disposition: normal
Date: 2026-09-11
Decision source: current user, this task (planning only).

## ゴール

PC-9821V13 / 64MB RAM / CF-IDEでインストールが行える。構成は2026-09-11ユーザー回答で確認済み。

## 前提と手順

[ws003-p024](../phase024/phase.md)で通常loaderがビープ停止を越え、[ws003-p026](../phase026/phase.md)の管理コマンド配置を確認する。PCI/USB機器を用いる経路では[ws003-p027](../phase027/phase.md)の該当設定・実装を先行確認する。
既存[ws019](../../ws019/ws.md)とws019-p050のPC98 FATインストール成果を使い、ソース媒体・インストール先・既存データ・使用するtext/graphicモードを具体化する。閉鎖済みWS019を再開しない。PC98ネイティブの起動・パーティション方式を維持する。
合意した通常経路でインストーラを起動し、対象を正しく識別して配置処理を完了する。その後、インストール先からloader→kernel→root→init/loginと基本操作を確認する。

## 完了条件・未決入力

対象・source/target識別子・通常成果物hash・選択モード・実際の画面/ログ・インストール結果・インストール先からの起動結果を保存する。QEMUの旧受け入れだけではclearedにしない。
起動元/インストール先の具体的構成、使用モード、保持すべき領域は未確定。未知の破壊的操作や異常系の網羅試験を追加せず、具体的な実行範囲を先に決める。

## 共通の制約・実行境界

親: [ws003](../ws.md)。Primary Milestone: MG003。今回の指示は計画更新。Queue: none / 実装未承認。
[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則、`plan/standards/automation.md`に従う。非CのMake/Python/assemblyは現行規約とABI・配置・サイズ制約を守る。規約のローカル資料を未公開URLとして捏造しない。
`include/hal/hal.h`・HAL責務は別の明示指示なしに変更しない。RTL8822Bのライセンス分離を保持。既存変更を保持し、aggregate `make check`、commit、pushは行わない。
実装前に有限Queueの範囲・時間枠を決める。検証は変更箇所に対応するfocused checkと選択構成の `make -j16`。PC98は維持対象qemu-pc98、amd64はqemu-system-x86_64を使い、共有build/runtimeは直列。試験用媒体は使い捨て。実機書込み対象・起動方式は具体化してから扱う。
結果にはsource/config・image hash、コマンド、観測、未実施項目、残課題を記録。旧QEMU/実機証拠を現在の成果物の合格へ読み替えない。実機成功をQEMU成功で代用しない。

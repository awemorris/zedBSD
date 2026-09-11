# ws003-p026: PC98 QEMUの /sbin 配置修復

Phase ID: `ws003-p026`
Status: planned
Phase disposition: normal
Date: 2026-09-11
Decision source: current user, this task (planning only).

## 問題・到達点

ユーザーはQEMUで起動したPC98環境の `/sbin` が空であると確認した。rootfs生成・格納・マウントのどこで管理コマンドが失われるか特定し、選択された管理コマンドとインストーラを正しい場所へ配置する。[BUG-023](../../bugs/BUG-023.md)。

## 現行コードと調査手順

`Makefile` の `ZEDBSD_ROOTFS_TAR_RULE` は `/sbin` を作成する。`userland/base/init/Makefile` はinitの配置先をsbinとし、`userland/base/zedinst/Makefile` は `/sbin/zedinst` と `/sbin/zedinst-graphic` を宣言する。これは現行ソースの静的確認であり、ユーザーのイメージの内容を確認したものではない。
`platform/pc98/vmunix.mk` のI386_ARCH_FILES、`platform/pc98/rootfs.mk`、プログラム選択とdestination関数、staging→rootfs.img/tar→実際のroot/overlayを追跡する。設定、成果物hash、起動時root/overlayを記録し、別イメージ・古い成果物・配置漏れ・上書きマウントを区別する。原因に対応する最小修正を設計する。

## 受け入れ・依存

前提はPC98の対象QEMU構成と成果物の特定。実機IPL修復を待たず調査できる。
選択プログラムの配置表と生成イメージ内の実体・実行属性が一致し、同一イメージでQEMUの `/sbin` に存在する。init/loginを維持し、選択した `/sbin/zedinst` を起動できる。不要な全コマンド追加を解決策にしない。対象イメージ・設定が未特定ならその点を残し、原因を断定しない。
出力は[ws003-p029](../phase029/phase.md)のPC98インストール確認に渡す。

## 共通の制約・実行境界

親: [ws003](../ws.md)。Primary Milestone: MG003。今回の指示は計画更新。Queue: none / 実装未承認。
[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則、`plan/standards/automation.md`に従う。非CのMake/Python/assemblyは現行規約とABI・配置・サイズ制約を守る。規約のローカル資料を未公開URLとして捏造しない。
`include/hal/hal.h`・HAL責務は別の明示指示なしに変更しない。RTL8822Bのライセンス分離を保持。既存変更を保持し、aggregate `make check`、commit、pushは行わない。
実装前に有限Queueの範囲・時間枠を決める。検証は変更箇所に対応するfocused checkと選択構成の `make -j16`。PC98は維持対象qemu-pc98、amd64はqemu-system-x86_64を使い、共有build/runtimeは直列。試験用媒体は使い捨て。実機書込み対象・起動方式は具体化してから扱う。
結果にはsource/config・image hash、コマンド、観測、未実施項目、残課題を記録。旧QEMU/実機証拠を現在の成果物の合格へ読み替えない。実機成功をQEMU成功で代用しない。

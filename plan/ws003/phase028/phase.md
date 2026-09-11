# ws003-p028: Let's Note LX6 USB起動のbootパーティション識別

Phase ID: `ws003-p028`
Status: planned
Phase disposition: normal
Date: 2026-09-11
Decision source: current user, this task (planning only).

## 問題・到達点

LX6のUSB起動でカーネルがbootパーティションを判別できずinitを起動できないというユーザー報告。[BUG-025](../../bugs/BUG-025.md)。正しい起動媒体・パーティションを同定してrootを組み立て、init/loginとインストーラ起動に到達する。

## 調査手順・対象

起動モード(BIOS/UEFI)、ソース・設定・USBイメージhash、`zedbsd.cfg`、ディスク/パーティション識別子、loader handoffとkernelの列挙・選択・マウントの最後の成功点を記録する。これらの詳細ログは未取得。USB媒体が未列挙、identity不一致、選択のタイミング、boot0/loader origin、root/overlay不足を切り分ける。
対象候補: `bootloader/uefi/volume-discovery.c` とconfig/handoff、該当BIOS経路、`src/kern/boot.c`、`src/kern/block-identity.c`、`src/kern/vfs.c`、関係するUSB/partition列挙。`kern_boot_source_context_mount` はboot0省略時にloader originを使う。実際の停止段階を確認して対象を絞る。
BUG-017の媒体列挙順観測、既存[ws003-p021](../phase021/phase.md)のSV7 GPT/USB-root修正は比較資料。根拠なしに同一原因・再発・既修正と扱わない。固定sda名や無条件sleepで起動を合わせない。

## 受け入れ・依存

特定した原因に対応するfocused回帰と、同一通常成果物のLX6実機USB→boot媒体解決→root→init/login→インストーラ起動を確認する。関連する現行QEMU起動も維持。実機ログ/操作が必要な段階を明記し、他機種の独立作業は待たせない。
出力は[ws003-p031](../phase031/phase.md)へ。LX6の起動モード・対象媒体・ログは実行計画の未決入力。

## 共通の制約・実行境界

親: [ws003](../ws.md)。Primary Milestone: MG003。今回の指示は計画更新。Queue: none / 実装未承認。
[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則、`plan/standards/automation.md`に従う。非CのMake/Python/assemblyは現行規約とABI・配置・サイズ制約を守る。規約のローカル資料を未公開URLとして捏造しない。
`include/hal/hal.h`・HAL責務は別の明示指示なしに変更しない。RTL8822Bのライセンス分離を保持。既存変更を保持し、aggregate `make check`、commit、pushは行わない。
実装前に有限Queueの範囲・時間枠を決める。検証は変更箇所に対応するfocused checkと選択構成の `make -j16`。PC98は維持対象qemu-pc98、amd64はqemu-system-x86_64を使い、共有build/runtimeは直列。試験用媒体は使い捨て。実機書込み対象・起動方式は具体化してから扱う。
結果にはsource/config・image hash、コマンド、観測、未実施項目、残課題を記録。旧QEMU/実機証拠を現在の成果物の合格へ読み替えない。実機成功をQEMU成功で代用しない。

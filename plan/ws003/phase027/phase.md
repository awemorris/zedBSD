# ws003-p027: PC98 menuconfigのPCI・USB選択

Phase ID: `ws003-p027`
Status: planned
Phase disposition: normal
Date: 2026-09-11
Decision source: current user, this task (planning only).

## 問題・到達点

PC98でPCI・USBドライバをmenuconfigから選択できないというユーザー報告。[BUG-024](../../bugs/BUG-024.md)。PC98で対応するドライバを選択し、保存・再読込み・build設定まで反映できるようにする。

## 現行コードと調査手順

`config/drivers/pci.drivers` と `usb.drivers` の主要項目はplatforms=`i386,amd64`でpc98を含まない。`tools/menuconfig.py` のapplies/option_rowsがこれを除外し、normalizeも非対応キーをnにする。`config/ci/config-pc98.mk` はPCI/USB項目をnに固定する。除外経路を静的に確認済みだがUI操作・修正・buildは未実施。
一覧ファイル、menuconfigのtarget正規化、Makeの条件付きソース列挙、`src/kern/platform/pc98.c` とPCI/USB初期化・割込み・DMA条件を照合する。既存の対応実装がある選択肢の範囲を確定する。表示だけ有効にして動作対応を装わない。

## 受け入れ・未決点

対象ドライバの選択・保存・再読込みが一致し、ON/OFFに応じて期待するbuild構成になる。既存PC98 IDE/console構成とi386/amd64の選択を維持する。設定保存と対応構成のfocused buildを確認する。PC98の既定値や全ドライバ一律有効化を無断で変更しない。
新しいPCI/USB実機対応が必要と判明した場合は範囲を具体化してからQueue化する。今回の計画だけで全PCI/USB機器のbring-upを引き受けない。実際に使うPCI/USB機器と必要ドライバは未確定。
[ws003-p029](../phase029/phase.md)では利用する機器に必要な設定成果を消費するが、PCI/USBを使わないIDE経路を不要なUSB試験で止めない。

## 共通の制約・実行境界

親: [ws003](../ws.md)。Primary Milestone: MG003。今回の指示は計画更新。Queue: none / 実装未承認。
[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則、`plan/standards/automation.md`に従う。非CのMake/Python/assemblyは現行規約とABI・配置・サイズ制約を守る。規約のローカル資料を未公開URLとして捏造しない。
`include/hal/hal.h`・HAL責務は別の明示指示なしに変更しない。RTL8822Bのライセンス分離を保持。既存変更を保持し、aggregate `make check`、commit、pushは行わない。
実装前に有限Queueの範囲・時間枠を決める。検証は変更箇所に対応するfocused checkと選択構成の `make -j16`。PC98は維持対象qemu-pc98、amd64はqemu-system-x86_64を使い、共有build/runtimeは直列。試験用媒体は使い捨て。実機書込み対象・起動方式は具体化してから扱う。
結果にはsource/config・image hash、コマンド、観測、未実施項目、残課題を記録。旧QEMU/実機証拠を現在の成果物の合格へ読み替えない。実機成功をQEMU成功で代用しない。

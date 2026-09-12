# Future Work

現在のPriority・実行Queueから外した将来項目。着手対象として選び直した時点で計画を更新する。
WS/Phase IDと完了済みの実績は消去・再利用しない。

| ID | 項目 | 保持する内容・再開点 |
| --- | --- | --- |
| F-001 | bridge | [旧WS011-p004](ws011/phase004/phase.md)からbridgeだけを移管。L2転送、member所有権、設定・永続化を独立して設計する。VLANは含めない。 |
| F-002 | [WS013：CPAR](ws013/ws.md) | Runtimeの名前空間・隔離、CLI/build、サービスコンテナを将来へ。完了済みp002〜p006のブート設定基盤は現行機能として維持する。 |
| F-003 | [WS015：μITRONリアルタイム領域](ws015/ws.md) | 互換プロファイル、RT/POSIX境界、常駐実行、通信・障害・時間保証の設計を将来へ移管。 |

VLANは2026-09-09ユーザー指示によりキャンセル。Future Listにも残さない。
旧MB-010のVLAN/bridge一括保留は終了し、bridgeはF-001として扱う。

## F-004: WS003終了に伴う未完了事項の保留（2026-09-12）

ユーザーがPPCを新規移植として独立WSへ移すよう指示し、WS003を閉じて再利用しないことを指定した。その他の未完了は「未完了のまま保留事項へ移し、WS003内のPhaseは終了する」と明示。目標達成や試験PASSを追加する判断ではない。

状態: deferred / 未完了。再検討時は具体的な単一目標の新WSを作る。インストーラ実機動作を再選択する場合もWS003は使わない。fg004は現在Focusから保留へ移す。

- [ws003-p001](https://github.com/awemorris/zedBSD/issues/67): ws003-p001: Latitude 5320 hardware inventory。未完了を保持。
- [ws003-p003](https://github.com/awemorris/zedBSD/issues/69): WS003 Phase 003: Latitude xHCI capability/MMIO bring-up。未完了を保持。
- [ws003-p017](https://github.com/awemorris/zedBSD/issues/83): WS003 Phase 017: UEFI LoadOptions firmware compatibility。未完了を保持。既存のws013-p003へのsuperseded判断も保持。
- [ws003-p018](https://github.com/awemorris/zedBSD/issues/84): WS003 Phase 018: Latitude existing-FAT NVMe overlay installation and boot。未完了を保持。
- [ws003-p019](https://github.com/awemorris/zedBSD/issues/85): WS003 Phase 019: Latitude NVMe native installation and boot。未完了を保持。
- [ws003-p025](https://github.com/awemorris/zedBSD/issues/91): WS003 Phase 025: HAL clock-source split and amd64 SMP monotonic counter。未完了を保持。

<details>
<summary>ws003-p026: ws003-p026: PC98 QEMUの /sbin 配置修復（旧Phaseは未公開。保留内容の全文）</summary>

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

</details>

<details>
<summary>ws003-p027: ws003-p027: PC98 menuconfigのPCI・USB選択（旧Phaseは未公開。保留内容の全文）</summary>

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

</details>

<details>
<summary>ws003-p028: ws003-p028: Let's Note LX6 USB起動のbootパーティション識別（旧Phaseは未公開。保留内容の全文）</summary>

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

</details>

<details>
<summary>ws003-p029: ws003-p029: PC-9821V13 インストール実機受け入れ（旧Phaseは未公開。保留内容の全文）</summary>

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

</details>

<details>
<summary>ws003-p030: ws003-p030: Let's Note SV7 インストール実機受け入れ（旧Phaseは未公開。保留内容の全文）</summary>

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

</details>

<details>
<summary>ws003-p031: ws003-p031: Let's Note LX6 インストール実機受け入れ（旧Phaseは未公開。保留内容の全文）</summary>

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

</details>

<details>
<summary>ws003-p032: ws003-p032: 実機インストーラbring-up変更の最終規約確認（旧Phaseは未公開。保留内容の全文）</summary>

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

## 2026-09-12 インストーラ実機動作を独立WS化

ユーザー指示により[WS028](https://github.com/awemorris/zedBSD/issues/382)を新設。単一目標はPC98 V13、Latitude 5320、SV7、LX6でインストーラを実行し、インストール先から起動・loginできること。fg004をこのWSで再選択する。PPC移植は[WS027](https://github.com/awemorris/zedBSD/issues/374)に分離済み。WS003は閉鎖済み・再利用禁止。

NVMeが実機で動作していないというユーザー報告と、menuconfigにNVMe項目がないだけかもしれないという仮説をWS028へ記録。現行pci.driversには項目があり、amd64/i386の組込み・登録経路もある。実機使用config/imageとの一致と失敗境界は未確認。

Future Work F-004のうち旧WS003 p018/p019とp026-p032はWS028への引継ぎ対象とする。その他は保留のまま。元Phaseは閉じたまま保持し、新しい実行Phase・Queueはまだ作らない。

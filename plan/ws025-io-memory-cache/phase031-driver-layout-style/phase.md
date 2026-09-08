# ws025-p031: ドライバ配置・命名・コーディング規約の統一

日付: 2026-09-08

Phase ID: `ws025-p031`

Status: uncleared; q123 finished。リファクタリング後の最終確認が残る。p032〜p035の回帰修正は完了。

Parent: [WS025](../ws.md)

依存: 完了済み ws025-p026。p027–p030 の実装より先に本 Phase を完了する。
条件付き p027–p030 の採用条件は引き続き有効であり、本 Phase によって自動採用しない。
q122 は finished の履歴として保持し、本 Phase は q123 で実行する。

## 目的と合意

ドライバを機能・バス・プラットフォーム別に整理し、外部リンケージを持つ
ドライバ定義を `drv_` に統一する。[coding-style.md](../../coding-style.md) を
本 Phase の対象 C ソース・ヘッダに適用する。移動だけでなく既存の対象実装も
規約レビューする。動作、ディスク形式、UAPI、エラー、所有権、同期順序を維持する。

以下のツリーを目標とする。`pcat-gprahics.c` は合意済みの構成案どおり
`pcat-graphics.c` と綴る。独立した `pci-intel-ax211.c` と新しい `input.h` は作らない。
以前の未完の `gpra` 箇条書きから追加作業を推測せず、最終合意ツリーを正とする。

## 目標ツリー

```text
src/drivers/
├── disklabel/
│   ├── gpt.c
│   ├── mbr.c
│   ├── pc98.c
│   ├── pcat.c
│   ├── sun.c
│   └── x68k.c
├── ethernet/
│   └── dp8390.c
├── fs/
│   ├── fat.c
│   └── ufs.c
├── generic/
│   ├── console.c
│   ├── dma.c
│   ├── input.c
│   └── loop.c
├── isa/
│   └── ne2000.c
├── pci/
│   ├── pci-ehci.c
│   ├── pci-nvme.c
│   ├── pci-pcat.c
│   ├── pci-uhci.c
│   ├── pci-xhci.c
│   └── pci.c
├── platform/
│   ├── pc98/
│   │   ├── graphics/
│   │   │   ├── backend.c
│   │   │   ├── backend.h
│   │   │   ├── display-auto.c
│   │   │   ├── display-auto.h
│   │   │   ├── display-cirrus.c
│   │   │   ├── display-cirrus.h
│   │   │   ├── display-gdc.c
│   │   │   ├── display-gdc.h
│   │   │   ├── display-glyph.c
│   │   │   ├── display-glyph.h
│   │   │   ├── display.h
│   │   │   └── pc98-graphics.c
│   │   ├── pc98-busmouse.c
│   │   ├── pc98-ide.c
│   │   ├── pc98-ide.h
│   │   └── pc98-lgy98.c
│   ├── pcat/
│   │   ├── graphics/
│   │   │   ├── backend.c
│   │   │   ├── backend.h
│   │   │   ├── font.c
│   │   │   ├── font.h
│   │   │   ├── pcat-graphics.c
│   │   │   ├── vgafont.c
│   │   │   └── vgafont.h
│   │   ├── pcat-ide.c
│   │   └── ps2-mouse.c
│   ├── rpi4/
│   │   ├── rpi4-sdhci.c
│   │   └── rpi4-sdhci.h
│   ├── sun4u/
│   │   ├── sun4u-cmd646.c
│   │   └── sun4u-cmd646.h
│   └── x68k/
│       ├── x68k-mb89352.c
│       ├── x68k-mb89352.h
│       ├── x68k-spc-disk.c
│       └── x68k-spc-disk.h
├── usb/
│   ├── usb-cdc-ecm.c
│   ├── usb-cdc-ncm-net.c
│   ├── usb-cdc-ncm.c
│   ├── usb-hid-checkpoint.c
│   ├── usb-hid.c
│   ├── usb-rtl8822bu.c
│   ├── usb-storage.c
│   └── usb.c
└── wifi/
    ├── intel-ax211/
    │   ├── intel-ax211-assoc.c
    │   ├── intel-ax211-assoc.h
    │   ├── intel-ax211-boot.c
    │   ├── intel-ax211-boot.h
    │   ├── intel-ax211-bss.c
    │   ├── intel-ax211-bss.h
    │   ├── intel-ax211-command.c
    │   ├── intel-ax211-command.h
    │   ├── intel-ax211-dma.c
    │   ├── intel-ax211-dma.h
    │   ├── intel-ax211-firmware.c
    │   ├── intel-ax211-firmware.h
    │   ├── intel-ax211-init.c
    │   ├── intel-ax211-init.h
    │   ├── intel-ax211-internal.h
    │   ├── intel-ax211-key.c
    │   ├── intel-ax211-key.h
    │   ├── intel-ax211-mmio.c
    │   ├── intel-ax211-mmio.h
    │   ├── intel-ax211-pci-mmio.c
    │   ├── intel-ax211-pci-mmio.h
    │   ├── intel-ax211-protocol.c
    │   ├── intel-ax211-protocol.h
    │   ├── intel-ax211-runtime-start.c
    │   ├── intel-ax211-runtime-start.h
    │   ├── intel-ax211-runtime.c
    │   ├── intel-ax211-runtime.h
    │   ├── intel-ax211-rx.c
    │   ├── intel-ax211-rx.h
    │   ├── intel-ax211-scan-session.c
    │   ├── intel-ax211-scan-session.h
    │   ├── intel-ax211-scan.c
    │   ├── intel-ax211-scan.h
    │   ├── intel-ax211-transport-backend.c
    │   ├── intel-ax211-transport-backend.h
    │   ├── intel-ax211-transport.c
    │   ├── intel-ax211-transport.h
    │   ├── intel-ax211-tx-ring.c
    │   ├── intel-ax211-tx-ring.h
    │   ├── intel-ax211-tx.c
    │   ├── intel-ax211-tx.h
    │   └── intel-ax211.c
    └── rtl8822b/
        ├── rtl8822b-internal.h
        ├── rtl8822b-security.c
        └── rtl8822b.c
```

[全ソースの移行表](source-map.tsv) に、計画時点の139ファイルの移動・統合先を
個別に記録した。同じ移行先を持つファイルは内容を統合する。実装開始時に差分を
再確認し、後から追加されたソースを取りこぼさない。

## 統合・宣言の設計

- `input-*.c` は `generic/input.c` へ統合する。既存の `include/kern/input-*.h`
  は利用者を調べて必要な外部宣言を維持・改名し、私有宣言は C 内に置く。
  `include/uapi/zedbsd/input.h` の ABI は維持する。
- `hid-report.c` は `usb/usb-hid.c` へ統合する。parser の外部利用を確認し、
  USB 内部だけであれば static 化と不要な宣言の削除を行う。
- 全 `src/drivers/**/*.inc` を所有する C に取り込む。DMA vector、FAT batch、
  UFS、xHCI SG、RTL8822B table を対象とし、定義順・macro 有効範囲・配列内容を維持する。
- `pci-nvme-{io-lifecycle,lifecycle,shutdown-lifecycle}.h` は `pci/pci-nvme.c` に統合する。
  `include/drivers/` の公開 NVMe protocol/API ヘッダまで無条件に消さない。
- `pci-intel-ax211.c` の接続・登録処理は `wifi/intel-ax211/intel-ax211.c` へ統合する。
  既存の専門モジュールを利用し、初期化順、PCI ID、IRQ/DMA、復旧、登録の重複を検証する。
- `disklabel/pc98-auto.c` は `pc98.c` に統合するが、形式読取りと自動選択の役割は維持する。
- 統合で衝突する static 関数、型、macro、変数は用途が分かる名前に改名する。
  include guard を残すだけで重複定義を隠さず、宣言を整理する。
- 公開ヘッダの配置全体を機械的に作り直すことは本合意に含めない。
  既存公開ヘッダの宣言と利用者を更新し、不要となる私有宣言のみ削除する。

## UFS と独立した mkfs

カーネル側の `fs/ufs/` 全体を `fs/ufs.c` に統合する。巨大な単一ファイルを許容し、
内部を規約の順序と意味のまとまりで整理する。journal、snapshot、allocation、
replay、xattr、namespace の所有権・永続化順序を変えない。

`mkfs` は他 OS に取り込めるユーザーランドツールとして独立させる。
`userland/base/mkfs/` に `ufs-disk.h`、`ufs-endian.c/.h`、`ufs-super.c/.h` を
コピーして取り込み、既存 `ufs-format.c` と組み合わせる。カーネルソースへの
include とビルド依存を除去する。共有ディレクトリや巨大なカーネル C の条件付き
コンパイルによる共用にはしない。コピー元・ライセンスを保持し、以後は独立管理する。

必要な型・定数・codec が推移的にカーネルへ依存していないことを調べる。
コピー時の形式互換性を fixture と生成イメージのカーネル読取りで検証する。
userland 内の共通コードへの依存は明示し、カーネルツリーなしのホストビルドを行う。
ドライバの `drv_` 規則は独立したユーザーランド codec へ強制しない。

## 外部シンボルと未使用コード

実装前に全ドライバの外部定義・宣言・参照を棚卸しする。関数だけでなく
公開データと登録テーブルも対象とし、次を明示的に改名する。

- `console_device_register` → `drv_console_device_register`
- `partition_scheme_{gpt,mbr,pc98,pc98_auto,pcat_auto,sun,x68k}`
  → `drv_partition_scheme_{gpt,mbr,pc98,pc98_auto,pcat_auto,sun,x68k}`
- その他の非 `drv_` 外部定義も同様に改名する。内部専用なら static 化する。

文字列置換だけでなく、関数ポインタ、機種別登録、assembly、linker、ホスト fixture、
テストによる直接 include/ソース抽出、生成処理を追跡する。
static 名、型、macro を一律に外部シンボル扱いしない。外部 ABI に関わる例外が
見つかれば理由と処置を記録し、互換性を壊す改名を隠れて行わない。

未使用コードの削除は全機種のビルド選択と間接参照を確認してから行う。
amd64 でリンクされないだけでは削除しない。判断できないものは残して理由を記録する。
削除対象・根拠・影響する fixture を一覧化する。

## ユーザーランドの配置

- `nettest` のソース、パッケージ登録、イメージ生成の `--nettest` 専用経路を削除する。
  現行の実行スクリプトに利用があれば通常の診断コマンド等に置換して検証範囲を維持する。
- `cxref` と `cflow` は維持し、インストール先を `/lib/debug/` に変更する。
  通常 image と standalone install の両方を更新する。`/bin` に旧コピーを残さず、
  スクリプトは新しい明示パスを使う。コマンドの挙動は変更しない。
- `/sbin` は現行生成 rootfs に存在し25コマンドを確認済み。実機で空に見えた原因は
  未特定。配置の大規模変更は加えず、fresh rootfs に必要コマンドが入ることを確認する。

## コーディング規約の適用

[coding-style.md](../../coding-style.md) 全体を正とし、特に以下をレビューする。

1. modeline・著作権・説明、tabs/幅8、macro/type/変数/宣言/public/static の順。
2. 関数定義の改行、static 前方宣言、公開関数と static 関数の説明。
3. ANSI C の関数先頭宣言、for 内宣言やスコープ限定ブロックの除去。
4. 意味のまとまりと空行、判断・loop・switch・return の目的コメント。
5. fallible call の個別評価、短絡順序の維持、明示的なエラー判定と戻り値。
6. goto の解消は ownership 境界の helper 化などで行い、逆順解放、lock、IRQ 状態、
   refcount、DMA lifetime、volatile/MMIO の評価回数・順序を維持する。
7. braces、引数改行、オブジェクトごとの初期化、テスト専用環境変数の不使用。

機械整形だけで意味保存を判定しない。規約適用に意味変更リスクがある箇所は個別に
レビューし、必要な例外は根拠を記録する。無関係なカーネル全体の整形へ拡大しない。

## 実装手順と中間確認

1. 現在の source/config、dirty 差分、対象機種、外部シンボル、fixture 参照を記録する。
2. 先に mkfs のコピーとビルド独立化を完了し、生成形式の一致を確認する。
3. 移行表に従いフォルダ移動・ファイル統合・ビルド定義の追随を行う。
   入力/HID、UFS/FAT、DMA/NVMe/xHCI、Wi-Fi の単位で衝突と登録を確認する。
4. 外部シンボルと宣言・利用者を揃え、対象ドライバ全体の規約を適用する。
5. nettest 削除と debug コマンドの再配置を行う。
6. 後述の検証を行い、結果・未検証機種・残る例外を results.md に記録する。

移動・統合・改名・制御フロー整形の差分をそれぞれ追跡できる記録を残す。
同じ共有ツリーで build/test/runtime と production 編集を同時に行わない。

## 受け入れと完了条件

- 目標ツリーと移行表を照合し、旧ディレクトリ、全 .inc、統合対象の私有ヘッダ、
  src/drivers 直下の C が残っていない。古いパスの有効なビルド参照がない。
  過去の受け入れログは改竄せず、新しい対応表から追跡する。
- 全ドライバの外部定義をソース解析と利用可能な機種別 object の symbol 表で照合し、
  非 drv_ 残件と必要な例外を列挙する。条件付きコードも確認する。
- supported x86 の amd64/pcat/pc98 を `make -j16` で直列ビルドする。
  その他の機種はビルド定義・include・登録を確認し、使用可能な toolchain で
  該当ビルドを実施する。実行不可の検証は理由付きで記録し PASS としない。
- 既存の入力/HID、disklabel、DMA/SG、USB/NVMe lifecycle、UFS/FAT、AX211/RTL8822B
  の focused host fixture を移行後の本体に接続して実行する。既存 sanitizer gate も維持する。
  ソース抽出テストのパス修正だけでなく、意図した本体を実行していることを確認する。
- 既存 FS50 と Wi-Fi30 の受け入れを完走する。native QEMU USB-root/NVMe で
  起動、read/write/fsync、再起動後の内容、終了処理の代表回帰を確認する。
  入力/console の登録と基本入力も既存の利用可能なテスト環境で確認する。
- mkfs をカーネルソースなしでホストビルドし、生成 UFS の形式検査とカーネルでの
  mount/read/write を確認する。rootfs 内の mkfs もビルドする。
- fresh rootfs と standalone install で `/lib/debug/cxref`、`/lib/debug/cflow` を確認し、
  小さな C 入力で動作確認する。旧 `/bin` 配置と nettest がなく、/sbin の管理コマンドがある。
- coding-style.md の checklist と `git diff --check` を確認する。統合に伴う機能変更を
  隠さず、既存 p026 の通常設定・永続化契約を維持する。通常 artifact に復帰する。

新しい形式だけをなぞる大量のテストは追加せず、既存 gate を再利用する。
不足する統合境界・所有権・形式互換性だけ focused fixture を補う。
物理機器で今回のコードを試していない場合は、過去の user-accepted 判定と区別する。

## 成果物と再開

本 phase.md、source-map.tsv、production/build/test の変更、実行後の results.md、
外部シンボルと規約例外・削除判断の記録を成果物とする。
再利用 fixture は WS025/tests、使い捨て診断は WS025/temp に置く。
`.internal/` を参照せず、commit と aggregate `make check` は行わない。

q123 で実行する。90 active minutes ごとに進捗と証拠を確認する。
途中終了時は完了済みの単位、失敗箇所、通常 artifact の状態、残条件を残す。
p031 完了後も p027–p030 はそれぞれの採用条件を満たした範囲で選択する。

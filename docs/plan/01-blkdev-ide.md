# Phase A: ブロックデバイス層 + PC-98 IDE ドライバ

前提: `docs/plan/00-overview.md` を読了していること。dev ブランチで作業。

## A-0. 目的と非目的

**目的**: ランタイムのディスク I/O を BIOS ゲートウェイから native IDE ドライバに
置き換える。カーネル側に blkdev / パーティションの2つの薄いインタフェースを立て、
ドライバを `drivers/` に置く体制を作る。

**非目的**(このフェーズではやらない):
- キーボード・時計・チェインブートのゲートウェイ置き換え(Phase C)
- 割り込み駆動 I/O(ポーリング PIO でよい。IRQ 化は Phase D)
- HAL の導入(Phase B)。このフェーズは現行の Boots 構造のまま行う
- SCSI / FDD / PC-AT

## A-1. 現状の確認(作業前に必ず実行)

```bash
cd ~/boots && git branch --show-current   # dev であること
grep -n "readsec\|writesec" platform/pc98/stage2.c | head
```

現状のディスク経路(アンカー: `grep -n "static int readsec" platform/pc98/stage2.c`):

- `readsec()/writesec()`(stage2.c 266行付近)が `BOOTS_BIOS_DISK_READ/WRITE`
  ゲートウェイサービスを1セクタずつ呼ぶ
- `scanparts()`(313行付近)が LBA1 を読み、PC-98 パーティションテーブル
  (32バイトエントリ×16)を CHS→LBA 変換(`chs()` 293行付近)しながら解釈する
- CHS ジオメトリは Stage 1 が BIOS SENSE(INT 1Bh AH=84h)した値が
  handoff のデバイステーブル(`struct boots_device` の heads/sectors)経由で届く
- `disk_volume_read/write`(351行付近)が fs 層の `boots_volume` に接続している
- M9 書き込みテスト(1195行付近, `BOOTS_M9_WRITE_TEST`)も readsec/writesec を使う

## A-2. 新しいインタフェース

### A-2-1. `core/blkdev.h` + `core/blkdev.c`(新規)

```c
/*
 * Boots block device interface
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef BOOTS_BLKDEV_H
#define BOOTS_BLKDEV_H

#include <stdint.h>

#define BOOTS_BLKDEV_MAX 8
#define BOOTS_BLKDEV_NAME_MAX 8

enum boots_blkdev_result {
	BOOTS_BLKDEV_OK = 0,
	BOOTS_BLKDEV_IO_ERROR,
	BOOTS_BLKDEV_OUT_OF_RANGE,
	BOOTS_BLKDEV_READ_ONLY,
	BOOTS_BLKDEV_INVALID,
};

struct boots_blkdev {
	char name[BOOTS_BLKDEV_NAME_MAX];      /* "ide0" .. */
	uint16_t sector_size;                  /* 512 */
	uint64_t sector_count;
	/* CHS geometry for partition-table interpretation.  Zero when the
	 * device has no meaningful geometry. */
	uint16_t heads;
	uint16_t sectors_per_track;
	enum boots_blkdev_result (*read)(struct boots_blkdev *dev,
					  uint64_t lba, uint32_t count,
					  void *buffer);
	enum boots_blkdev_result (*write)(struct boots_blkdev *dev,
					   uint64_t lba, uint32_t count,
					   const void *buffer);
	enum boots_blkdev_result (*flush)(struct boots_blkdev *dev);
	void *private_data;
};

int boots_blkdev_register(struct boots_blkdev *dev);
unsigned boots_blkdev_count(void);
struct boots_blkdev *boots_blkdev_get(unsigned index);
struct boots_blkdev *boots_blkdev_find(const char *name);

#endif
```

`blkdev.c` は固定配列 `BOOTS_BLKDEV_MAX` の登録簿だけ。ヒープ不使用。
`write` が NULL のデバイスは read-only(fs 層の流儀と同じ)。

### A-2-2. `core/partition.h` + `core/partition.c`(新規)

パーティションテーブルの「形式」をターゲット非依存の口にする(決定 D3):

```c
#define BOOTS_PARTITION_MAX 16
#define BOOTS_PARTITION_NAME_MAX 17

struct boots_partition {
	uint64_t start_lba;    /* パーティション先頭(ブートコード側) */
	uint64_t data_lba;     /* データ領域先頭(PC-98では start と別) */
	uint64_t sector_count; /* 0 なら不明(PC-98 形式はサイズを持たない) */
	uint8_t bootable;
	char name[BOOTS_PARTITION_NAME_MAX];
};

struct boots_partition_scheme {
	const char *name;   /* "pc98" / "mbr" */
	/* テーブルを読んで entries を埋め、個数を返す。負値はエラー。 */
	int (*scan)(const struct boots_partition_scheme *scheme,
		    struct boots_blkdev *dev,
		    struct boots_partition *entries, unsigned max_entries);
};

void boots_partition_set_scheme(const struct boots_partition_scheme *scheme);
int boots_partition_scan(struct boots_blkdev *dev,
			  struct boots_partition *entries,
			  unsigned max_entries);
```

`partition.c` は登録された1つの scheme に委譲するだけ(将来ターゲットごとに
プラットフォーム初期化が登録する。MBR 追加時も同じ口)。

### A-2-3. `platform/pc98/partition-pc98.c`(新規)

現在 `scanparts()` にある PC-98 形式の解釈をここへ移す:

- LBA 1(セクタサイズ 512 の場合。正確には `2048 / sector_size` … 現行実装は
  LBA1 固定なのでまず互換で LBA1 のままとし、コメントで注記)を読む
- 32 バイト×16 エントリ。`p[0]` の bit7 と `p[1]` の bit7 が bootable、
  `p+4` が start CHS、`p+8` が data CHS、`p+16..31` が名前
- CHS→LBA: `lba = (cyl * heads + head) * sectors_per_track + sect`
  — ジオメトリは `dev->heads / dev->sectors_per_track` を使う
  (現行 `chs()` と同じ式。stage2.c 293行付近から移植)

## A-3. PC-98 IDE ドライバ `drivers/ide-pc98.c`(新規)

### レジスタマップ(グラウンドトゥルース: `~/qemu-pc98/hw/ide/pc98-ide.c` 冒頭コメント)

| ポート | 意味 |
|---|---|
| 0x430 | 接続/構成レジスタ(読み: bit0=標準レイアウト) |
| 0x432 | **バンク選択**(2つの ATA チャネルを1つのレジスタブロックに多重化。bit0=バンク) |
| 0x640 + reg*2 | ATA コマンドブロック(reg 0..7: data, error/feat, nsect, lbal, lbam, lbah, drive/head, status/cmd) |
| 0x74C | 代替ステータス / デバイス制御 |
| 0x74E | (制御ブロック続き) |

IRQ は 9(Phase D まで使わない)。詳細挙動・確認事項は必ず
`~/qemu-pc98/hw/ide/pc98-ide.c` と
`~/linux-pc98/external/kernel/linux-7.1/drivers/ata/pata_pc9800.c` を読んで合わせること。

### 実装内容

1. **probe**: バンク 0/1 × master/slave の最大4ユニット。
   各ユニットに IDENTIFY DEVICE (0xEC) を発行し、応答したものを登録。
   - status 0xFF/タイムアウトは不在
   - IDENTIFY の word 49 bit9 で LBA 対応判定。対応なら word 60-61 が総セクタ数
   - 非対応なら word 1/3/6(cyl/heads/spt)から CHS アドレッシングにフォールバック
   - **CHS ジオメトリ**: blkdev に載せる heads/sectors_per_track は
     handoff のデバイステーブル(BIOS SENSE 値)を優先する。
     パーティションテーブルは BIOS が書いた CHS で記録されているため、
     IDENTIFY のネイティブジオメトリと食い違うことがある(重要)
2. **read/write**: READ SECTORS (0x20) / WRITE SECTORS (0x30)、PIO ポーリング。
   - BSY クリア待ち → drive/head 選択 → レジスタ設定 → コマンド →
     セクタ毎に DRQ 待ち → data ポート 0x640 から 16bit×256 回 insw/outsw 相当
   - タイムアウトは十分大きなループ回数(参考: qemu モデルは即応)+ ERR ビット検査
   - マルチセクタは1コマンドで nsect 指定(最大 256=nsect 0)。
     count がそれを超えるときは分割
3. **登録**: `boots_ide_pc98_init(const struct boots_device *bios_devs, unsigned n)`
   を公開し、platform 初期化から呼ぶ。BIOS デバイステーブルとの対応付け:
   - handoff の `bios_id` 下位ビットがユニット番号(IDE は DA/UA 0x80+n)。
     bank = n >> 1, drive = n & 1 と対応(qemu モデルで検証すること)
   - blkdev 名は "ide0".."ide3"

### Makefile

`platform/pc98/platform.mk` の `STAGE2_OBJS` に
`$(BUILD)/core/blkdev.o $(BUILD)/core/partition.o $(BUILD)/drivers/ide-pc98.o
$(BUILD)/$(PC98)/partition-pc98.o` を追加。
`drivers/*.c` 用の暗黙ルールは既存の `$(BUILD)/%.o: %.c` が効くので追加不要。
**リンカスクリプトの low セグメント**に新オブジェクトを追加すること
(`platform/pc98/stage2.ld` の .text.low ほか各所へ `*blkdev.o` `*partition.o`
`*ide-pc98.o` `*partition-pc98.o` を追記。ディスク I/O はカーネルロード中に
実行されるので **low 必須**)。

## A-4. stage2.c の接続替え

1. `readsec()/writesec()` の中身を blkdev 呼び出しに置き換える
   (シグネチャは維持すると差分が小さい: `boots_device*` から対応する
   `boots_blkdev*` を引く対応表を boot 時に作る)。
   `BOOTS_BIOS_DISK_READ/WRITE` の `call()` は**このフェーズで呼び出しゼロ**になる
   (サービス自体の削除は Phase C。stage1 は触らない)
2. `scanparts()` を `boots_partition_scan()` 呼び出しに書き換え、
   結果を既存の `parts[]` 形式に詰め替える(`register_scanned_disk` 等の
   下流は無改造で済む)
3. platform 初期化(`boots_main` の初期化部)で
   `boots_ide_pc98_init()` と `boots_partition_set_scheme(&pc98_scheme)` を呼ぶ
4. M9 テスト(`BOOTS_M9_WRITE_TEST`)は readsec/writesec 経由のまま動くはず。
   動作意味が「BIOS 書き込み検証」から「IDE 書き込み検証」に変わるので、
   stage2.c 内の該当コメントを更新する

## A-5. ホストテスト(新規)

`tests/blkdev-host-test.c`: メモリ上の偽 blkdev を登録し、
- 登録/検索/上限、範囲外 LBA、read-only(write==NULL)
- `partition-pc98.c` を偽 blkdev 上のテーブル画像でテスト:
  正常16エントリ / 空エントリ / bootable ビット / CHS→LBA 変換値
  (期待値は現行 `chs()` の式で手計算した値をリテラルで書く)

Makefile の `HOST_TEST_BINARIES` に追加(`fat-host-test` の規則を雛形に。
ホスト cc でビルドできるよう、blkdev/partition のソースはフリースタンディング
非依存に保つ)。

## A-6. 検証

```bash
cd ~/boots
make ARCH=pc98 check 2>&1 | tail       # 既知の32bitリンク失敗以外が全PASS
make ARCH=pc98 all 2>&1 | grep -E "BOOT.SYS|IO.SYS|bytes"
grep -n "BOOTS_BIOS_DISK_READ\|BOOTS_BIOS_DISK_WRITE" platform/pc98/stage2.c
# ↑ stage2.c に残る参照が 0 件であること(abi.h の定義は残ってよい)
```

QEMU 環境がある場合のみ:
```bash
make ARCH=pc98 hdd-image && scripts/test-hdd-boot.sh    # IDE 経由で起動する
scripts/test-noct-file.sh                               # fs 読み書きが IDE 経由
scripts/test-bios-write.sh                              # M9(名称は据置でよい)
```
QEMU の machine は pc98 の IDE モデル(`~/qemu-pc98/hw/ide/pc98-ide.c`)を使う
構成であること。無ければ「QEMU 未検証」と報告して終了。

## A-7. コミット単位の目安

1. `Add the block device and partition scheme interfaces`(core/ + テスト)
2. `Add the PC-98 IDE PIO driver`(drivers/ + platform 登録)
3. `Route disk I/O through the block device layer`(stage2.c 接続替え)

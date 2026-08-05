# Boots カーネル化 計画: 全体像

対象: `~/boots`(dev ブランチ)。main へのマージは人間が行う。

## 0. 実行者への規則

- フェーズ文書(01-, 02-, 03-…)を番号順に実行する。各フェーズ内の手順・検証も順番どおり。
- **コミットは dev ブランチのみ**。main には絶対にコミットしない。
  フェーズ内の意味のある単位でコミットしてよい(メッセージは既存コミットの流儀:
  英語・現在形・理由を本文に。`Co-Authored-By: Claude <モデル名> <noreply@anthropic.com>` を付ける)。
- 文書内の行番号・アンカーはズレることがある。**必ず grep アンカーで再確認**し、
  見つからなければ停止して報告する。
- 各フェーズ末尾の検証が全部通ってから次へ。通らないとき、チェックを緩めて通すのは禁止。
- 判断に迷ったら設計を変えず、停止して報告する。

## 1. ゴール

Boots を「BIOS 間借りのブートローダ」から「HAL 上の小さなカーネル」に育て、
PC-98 i386 と PC/AT i386 の両方で動かす。最終的に:

- 割り込み(IDT/PIC/PIT)とプリエンプティブなタスクスケジューリング
- ランタイムの BIOS コール全廃(native IDE / キーボード / 時計)
- Noct のマルチスレッド(Thread.*)が BOOT.SYS 内で動く
- Noct upstream に boots-i386 プリセット(静的ライブラリのみ、Boots カーネル API 使用)
- 将来: hal の space+task でプロセス、複数 VM

## 2. 確定済みの設計判断(変更禁止)

| # | 決定 |
|---|---|
| D1 | ランタイム BIOS コールは**全削除**。ディスクは native IDE ドライバ(後日 SCSI 追加可能な形) |
| D2 | **ドライバは HAL に置かない**。カーネルに薄いブロックデバイス IF を定義し、ドライバは `drivers/` に置き、プラットフォーム初期化が登録する |
| D3 | **パーティションテーブル解釈もインタフェース化**しターゲット依存に(PC-98 形式 / MBR)。CHS ジオメトリは 16bit ブートコードが BIOS SENSE してカーネルに渡す |
| D4 | ブートフローは全ターゲット共通: LBA0 → 先頭パーティションまでの空間 → パーティション PBR → IO.SYS 領域 → FAT16 から BOOT.SYS。ブートセクタ類は `bootsectors/<target>/` で管理。**HAL とカーネルは完全に 32-bit ELF の世界** |
| D5 | MT のアトミックはラッパ層で実装。**UP(単一CPU)前提の cli/popf 実装**により CPU 下限は i386(`xadd`/`cmpxchg` は 486+ のため使わない。SMP 対応時に lock 命令版へ差し替え)。`cmpxchg8b` 不要 |
| D6 | FB API は**リニアフレームバッファを前提にしない**。BeUI の「イメージと VRAM の分離」を維持し、FILL/LINE/PATTERN(MASK) FILL/DRAW_IMAGE のオペレーションベースで定義。実装が LFB でもよい |
| D7 | `NOCT_TARGET_PC98DOS` は**ノータッチ**で維持 |
| D8 | PC/AT の日本語表示は当面非サポート。ASCII のみ。フォントは起動時に VGA カードのフォント RAM(プレーン2)から吸い出し、失敗時は寛容ライセンスの埋め込みフォント(Spleen 等 BSD 系。CC BY-SA 系は避ける)にフォールバック |
| D9 | HAL は kcrt 非依存に自己完結させる(`hal_memcpy` 等を HAL 内に実装)。`hal/i386/task.c` の `malloc` はアロケータフック注入に変更 |
| D10 | HAL cons に属性 API を1つ追加: `hal_cons_set_attr(int fg, int bg)`(0-15/0-15。未対応ターゲットは縮退可) |
| D11 | チェインブート(メニューから他 OS 起動)は維持。exit トランポリンに PIC/IVT 復元を含める |
| D12 | FDD は BIOS 削除により将来の µPD765 FDC ドライバ待ち。FAT12 FDD マウント計画は保留 |
| D13 | IDE は初期実装ポーリング PIO、スケジューラ到着後に IRQ 化の2段階 |

## 3. 目標アーキテクチャ

```
NoctLang (boots-i386 preset: libnoct.a/libnoctapi.a, MT=ON, i386)
  api-thread → kapi スレッド    api-beui-boots → kapi fb/glyph
────────────────────────────────────────────
Boots カーネル  <boots/kapi.h>
  core/   fs, fat, env, namespace, blkdev IF, partition IF, カーネルロード
  kern/   スケジューラ(~/kt/sys/kern/sched.c 由来), kapi 実装
  drivers/ ide-pc98, kbd-pc98, fb-gdc, fb-cirrus, (将来 ide-pcat, scsi, fdc…)
  platform/<t>/ 初期化・ドライバ登録・パーティション形式登録
────────────────────────────────────────────
HAL  hal/include/hal/hal.h  (int/task/page/pmem/cons(+attr)/fb はオペレーション口のみ)
  hal/i386/ + bsp-pc98 | bsp-pcat
────────────────────────────────────────────
bootsectors/pc98  (16bit: IPL, PBR, IO.SYS=stage1。CHS SENSE と multiboot 情報構築)
bootsectors/pcat  (将来: ~/kt/boot/bootsect 由来)
```

## 4. フェーズ一覧

| フェーズ | 文書 | 内容 | 終了時の状態 |
|---|---|---|---|
| A | 01-blkdev-ide.md | blkdev IF + パーティション IF + PC-98 IDE(PIO) | ディスク I/O が native。BIOS はキー/時計/チェインのみ |
| B | 02-hal-integration.md | bootsectors/ 再編、HAL 自己完結化、higher-half 化、locore→hal_main→kernel_entry、IDT/例外 | HAL 上で全機能動作(まだシングルタスク) |
| C | 03-gateway-removal.md | native キーボード/時計、exit トランポリン、**BIOS ゲートウェイ削除** | ランタイム BIOS ゼロ |
| D | (C 完了後に作成) | kt sched 移植、PIT プリエンプション、IDE の IRQ 化、BeUI clock の tick 化 | プリエンプティブマルチタスク |
| E | (同上) | <boots/kapi.h> 整備、GDC/Cirrus/グリフを drivers/ へ、hal fb オペレーション新設 | カーネル API 確立 |
| F | (同上) | Noct boots-i386 プリセット(MT、atomic.h UP実装、api-thread kapi バックエンド)、noct.mk 廃止 | Thread.* が BOOT.SYS で動く |
| G | (同上) | platform/pcat + bootsectors/pcat + drivers/(ATA, 8042, VGA) | PC/AT で Boots が起動 |
| H | (将来) | space 分離、プロセス、複数 VM | — |

D 以降の詳細文書は C 完了後に作成する(B/C の実装結果で前提が変わるため)。

## 5. 参照実装(必ず活用すること)

| 対象 | 場所 |
|---|---|
| PC-98 IDE の挙動(**QEMU テストのグラウンドトゥルース**) | `~/qemu-pc98/hw/ide/pc98-ide.c` |
| PC-98 IDE Linux ドライバ | `~/linux-pc98/external/kernel/linux-7.1/drivers/ata/pata_pc9800.c` |
| PC-98 キーボード | `~/linux-pc98/.../drivers/input/keyboard/pc98kbd.c`, `~/qemu-pc98/hw/input/pc98-kbd.c` |
| PC-98 PIC | `~/qemu-pc98/hw/intc/i8259-pc98.c`, `hal/i386/bsp-pc98/pic.c` |
| ELF サブセットローダの手本 | `~/kt/boot/bootsect/nec-pc98.S`, `ibm-pcat.S` |
| スケジューラ | `~/kt/sys/kern/sched.c`(**Shift-JIS。取り込み時に UTF-8 変換**) |
| multiboot 受け取り側 | `hal/i386/locore.S`(EAX=0x2BADB002, EBX=mbi, 90バイトを 0x2000 へコピー) |

## 6. 既知の地雷(先に読むこと)

1. **`hal/i386/i386.h` の `ADDR_INIT_PT = 0x00020000` は BOOT.SYS low セグメント(0x20000)と衝突する。**
   Phase B で 0x90000 へ移動する(0x90000-0x9FFFF の 64KiB = PDT 4KiB + PT 15枚で
   15MiB をマップ可能。boot params 0x80000-0x8FFFF とは重ならない)。
2. HAL は higher-half(`SYS_START 0x80000000`)。BOOT.SYS の VMA は Phase B で
   0x80020000/0x80100000 になり、stage1 は `paddr = vaddr & 0x7fffffff` でロードする
   (`~/kt/boot/bootsect` の `btr eax, 31` と同じ規則)。
3. ゲートウェイが残る期間(A〜C 途中)に PIC をリマップすると、リアルモード BIOS の
   ISR が壊れる。Phase B ではゲートウェイ呼び出しの入口/出口で PIC を BIOS 配置に
   戻すブラケットを実装する(Phase C で丸ごと消える)。
4. この開発環境には `libc6-dev-i386`/PC-98 QEMU/mtools が無い。
   `stdio-fs-host-test` `libc-host-test` `softfloat-host-test` `noct-host-test` の
   リンク失敗は既知。QEMU 検証手順は「QEMU 環境があれば」の扱いで、無ければ
   未実行と報告する。

## 7. ガードレール

1. main ブランチにコミットしない。noct/ submodule の中身・参照を変更しない。
2. `NOCT_TARGET_PC98DOS` 関連(noct 側の beui-pc98-*.c, api-beui-pc98dos.c)に触れない。
3. BeUI のスクリプト向け API 面(BeUI.*、Key/Button 辞書)を変えない。
4. ~/kt からコードを取り込むときは UTF-8 に変換し、Boots の流儀(タブ、英語コメント、
   zlib ライセンスヘッダ)に合わせる。ロジックは変えない。
5. 検証をすり抜けるためにチェッカや ASSERT を緩めない。
6. `~/kt` `~/linux-pc98` `~/qemu-pc98` は読み取り専用。変更しない。

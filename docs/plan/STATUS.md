# 実装ステータスと Phase B ブロッカー

## 追記4: Phase B/C HAL boot flip の QEMU 検証完了

チェックポイント5〜6を `pc9821 -cpu 486` で完了した。BOOT.SYS は
`text_start=0x80021164` から起動し、QEMU monitor で paging 有効後の
`EIP=0x8002....`、`CR0.PG=1` を確認した。HAL console、native IDE、native
8251A keyboard、PIT tick、GDC/Cirrus BeUI、Linux exit trampoline が実行経路で動作した。

QEMU bring-up で判明した修正:

- higher-half の `__high_end` を物理化せず Noct arena の予約量に使っていたため、
  全 RAM profile で `insufficient script arena` になっていた。bit 31 を落として
  物理終端を計算するよう修正した。
- native keyboard event は Term 用 modifier snapshot を上位ビットに持つ。
  REPL line editor が key code 部分を取り出して Ctrl-C を認識するよう修正した。
- 20 MiB 以下の legacy raw image は H=4/S=17、40 MiB 級を含む通常の IDE
  image は H=8/S=17 で生成する。BusyBox release image も実機互換性を優先して
  H=8/S=17 とする。
- QEMU test profile を `pc9821 -cpu 486` に移行し、HDD 自動起動テストは一瞬の
  固定行ではなく専用 AUTOEXEC の完了 marker を text VRAM 全体から検査する。

QEMU 検証結果:

```text
make ARCH=pc98 all: PASS
noct-memory-host-test: PASS
test-hdd-boot.sh: PASS (fresh 16 MiB, H=4, PBR LBA 68)
test-noct-repl.sh: PASS (Ctrl-C returns to BOOTS.CFG)
test-beui-input.sh: PASS (1 s clock, key make/break)
test-beui-menu.sh: PASS (arrow/Enter and text restore)
test-beui-holoris.sh: PASS
test-autoexec-remacs.sh: PASS
Linux 7.1: hda2 ext4 root mounted, BusyBox init,
           I386-BUSYBOX-SUCCESS marker reached
MS-DOS 6.20: H=4-converted copy reached A:\\> prompt
```

`make ARCH=pc98 check` は従来どおり host の 32-bit `Scrt1.o` / `crti.o` / libgcc
不足で `stdio-fs-host-test` のリンク時に停止する。実行可能な
`noct-memory-host-test` は個別に合格した。

QEMU 本体には、BIOS の短い status poll 中にも file-backed request を完了させる
`hw/ide/pc98-ide.c` の `blk_drain()` 変更を未コミットで残す。`pio_aiocb`/BUSY 限定は
MS-DOS の command path を救えなかったため、PC-98 status read 前の drain とした。
`roms/pc98bios/ide.S` の H=4/H=8 境界も20 MiB級へ変更した。QEMU 差分は
別途レビュー対象。

Phase B/C と HAL boot flip は完了。未実装として残るのは固定ディスクの chain boot
(現在は明示的に unavailable と表示)など、次フェーズの機能である。

## 追記3: Phase B/C HAL boot flip の静的統合完了

`docs/plan/04-hal-boot-flip.md` のチェックポイント0〜4を dev に実装した。

- `b69c9b6`: B98S header分離、HAL/kernの階層保持make target化
- `5c4e89e`: 8251A native polling keyboard、正規化key+modifier event契約
- `b010bd7`: native GDC START/STOP、descriptor基準のIDE再probe、chain boot未対応表示
- `d2ddb45`: IDT/PIC/PIT初期化順、IRQ0 kernel tick、handoff検査、Linux paging-off trampoline
- `db623f0`: higher-half HAL entry、BOOT.SYS v3、stage1 bit31 mask、BeUI fb ownership proxy

静的検証結果:

```text
entry: 0x80021164 (text_start), B98S physical entry: 0x00021164
LOAD 0: offset 0x200, VAddr/PhysAddr 0x80020000, filesz 0xcbb4, memsz 0x404fc
LOAD 1: offset 0xce00, VAddr/PhysAddr 0x80100000, filesz 0x44c50, memsz 0x4a9f8
low physical margin to 0x80000: 0x1fb04
make ARCH=pc98 all: PASS
hal-pc98-compile / kern-compile: PASS
call(BOOTS_BIOS_ in stage2.c: 0
kbd-pc98-map-host-test: PASS
```

`noct-host-test` は既知の環境問題(`crt1.o`, `crti.o`, 32bit libgcc不在)でリンク不能。

QEMU起動ラダーは最初の境界で停止した。`pc9821 -cpu 486 -boot c` の有無どちらでも
POST画面(`MEMORY ... OK / BIOS DIAGNOSTICS OK / TAB: SETUP`)から進まず、
`PC98_IDE_TRACE=1` のIDE accessは0件だった。テストイメージは8 heads / 17 sectors、
LBA1先頭entryはMID `0xa1` / SID `0x91`。従ってstage1、physical text_start、HALは
まだ実行されていない。`docs/plan/04-hal-boot-flip.md` の中断条件に従い、読み取り専用の
`~/qemu-pc98` は変更していない。

残作業は、BIOSがIDE boot scanへ進む既知良好なQEMU設定または実機を得た後の
チェックポイント5〜6(起動、keyboard/BeUI/Linux、QEMU回帰)とchain boot実装。

最終更新: dev ブランチ作業セッション(自律実行)

## 完了

### Phase A — ブロックデバイス層 + PC-98 IDE ドライバ ✅
- `core/blkdev.{h,c}`: 固定表のブロックデバイス登録簿(ヒープ不使用)
- `core/partition.{h,c}` + `platform/pc98/partition-pc98.c`: パーティション形式の
  インタフェースと PC-98 実装(ファームウェア SENSE ジオメトリで CHS→LBA)
- `drivers/ide-pc98.c`: PIO ポーリングの PC-98 内蔵 IDE ドライバ
- `platform/pc98/stage2.c`: `readsec/writesec/scanparts` を blkdev/scheme 経由に
- `tests/blkdev-host-test.c`: 登録簿契約と CHS 演算のホストテスト
- **検証**: `make ARCH=pc98 all check` の実行可能分すべて PASS、BOOT.SYS 契約通過、
  `grep BOOTS_BIOS_DISK stage2.c` = 0 件
- コミット: 1745d70, c3bb9c8, ee6c5c0(+ マージ復旧 754f36f)

補足: マージ(0e07fdf)が ELF BOOT.SYS パイプライン(35031e4)を丸ごと落としていた。
754f36f で復旧済み。

## ブロック中

### Phase B — HAL compilable 化まで完了 / ブート配線は相談中 🟡

`hal/` は import 時点(58d1a7d)では**コンパイル不能な未完成ドラフト**だった。これはユーザ自身が別途(~/kt で)開発中のコードであり、自律実行での
大規模改変はユーザの作業と衝突するリスクが高い。かつ、このサンドボックスには
PC-98 QEMU が無く、ブート経路の検証ができない。よって**勝手に書き進めず停止**した。

判明した具体的欠落(`hal/` を読んで確認):

| 種別 | 内容 |
|---|---|
| 欠落ヘッダ | `hal/i386/{asm,pic,irq,clock}.h` が参照されるが存在しない |
| 未定義関数 | `cmain.c` が呼ぶ `i386_page_init/i386_int_init/i386_task_init/bsp_timer_init` がどこにも無い(実体は `i386_mem_init/int_init/task_init/clock_init` 等、名前が不一致) |
| 公開API乖離 | `hal/include/hal/hal.h` は `hal_irq_*/hal_task_*/hal_page_*/hal_cons_*` の POSIX風契約。実装は `int_init/task_init/cons_putc/pmem_alloc/univ_*`。両者が繋がっていない |
| 壊れたC | `bsp-pcat/irq.c` は `void (*isr_func)(void)[N];` 等の不正な宣言、`bsp_irq_send_eoi` 内で未定義の `irq_num` 参照など、未完成 |
| 型基盤 | `<sys/types.h>` 等 kt 依存。`HAL_ARCH_I386` マクロ前提 |

### さらに Phase B の設計上の未決事項(計画 02 の「調査ポイント」)

`hal/i386/locore.S` は現状 **物理 0x20000 から 256MB 分(64 PT = 256KB)** の
ページテーブルを構築する。計画では「`ADDR_INIT_PT` を 0x90000 へ移動」としたが、
0x90000 に 256KB 置くと 0xA0000 の VRAM を踏む。マップ量を 15MiB 程度に削るなら
0x90000 に収まるが、**これは「初期マップ範囲をいくら取るか」というユーザの HAL の
設計判断**であり、機械的な移動ではない。BOOT.SYS low セグメント(物理 0x20000)との
衝突は実在するため、いずれにせよ ADDR_INIT_PT は動かす必要がある。

## Phase B を再開するために必要な判断/入力(ユーザへ)

1. **HAL の完成主体**: HAL をコンパイル可能にする作業は、私が行ってよいか、
   それともユーザが ~/kt で仕上げてから Boots に再 import するか。
   前者なら、欠落ヘッダの新規作成・cmain の呼び出し名の統一・公開 API と実装の
   接続方針(hal.h をどこまで実装するか)の指針が要る。
2. **初期ページマップ範囲**: locore が構築する初期マップは何 MiB か。
   これで ADDR_INIT_PT の移動先が決まる。
3. **hal.h 契約のスコープ**: Boots が当面使うのは cons / int / task / timer / pmem /
   fb だけ。space/SMP/syscall/trap の広い契約は将来でよいか(= 実装を段階化してよいか)。

## ブロッカーに依存しない前進(このセッションで実施)

Phase C-2 のうち **HAL のブートに依存しない純ロジック**である PC-98 キーボードの
スキャンコード→正規化キー変換表を、独立モジュール `drivers/kbd-pc98-map.{h,c}` として
先行実装し、ホストテストで検証した(`tests/kbd-pc98-map-host-test.c`)。
実際の 8251A ドライバ(IRQ・リングバッファ)は Phase C/D で HAL の上に載せるが、
変換表は今フル検証できるため切り出した。参照: linux-pc98 pc98kbd.c のスキャン表、
既存 `stage2.c` の `key_to_scan()`(逆写像)、`<noct/beui.h>` の正規化キー namespace。

## 次にやるべきこと(優先順)

1. ユーザが上記 3 判断を返す → Phase B 再開(HAL を compilable に → higher-half →
   割り込み → tick)
2. Phase C の残り(8251A ドライバ本体、RTC/tick 時計、exit トランポリン、
   ゲートウェイ削除)
3. Phase D 以降の詳細計画を B/C 実装後に作成


---

## 追記: HAL を compilable 化(dddae7c)

ユーザ確認により、~/kt の `dc95e73 (PC98 works)` にコンパイル可能版が残っている
ことが判明。そこから復元し、i386 コア + PC-98 BSP を **freestanding / -Werror で
クリーンにコンパイル**する状態にした(`make ARCH=pc98 hal-pc98-compile` で常時検証。
`check` にも組み込み)。

- 復元: `irq.c`(ローカル割り込みロック・ISRタスク・タイマEOI)、`page.c` の pmem 半分、
  `univ.c`、`asm/irq/pic/clock.h` と `sys/` 公開ヘッダツリー
- 自己完結化: `lib.c` に crt_*(文字列/print/assert)。ヒープは `crt_set_allocator`
  注入(タスク生成前にカーネルが自分のアロケータを配線)
- cons 属性: `cons_set_attr(fg,bg)` 0-15。PC-98 は 3bit前景/背景無しに縮退、属性プレーンへ書込
- pmem: 固定物理アドレス常駐をやめ静的BSS化。デバイス窓(VRAM/ROM、15-16MBホール)は
  `pmem_reserve()` で除外(範囲管理のみ、ページテーブル操作なし)
- locore: 初期ページテーブルを固定低位(0x20000, low セグメントと衝突)から BSS へ移動。
  256MB を higher-half + identity の両方にマップ(旧ローダが渡した物理ポインタが
  paging 後も有効)、Cirrus アパーチャ 0xf0000000 も cache-disabled で identity マップ
- bsp-pcat はユーザの WIP(Phase G)として未着手

## ブート配線(Phase B 残り)— 相談したい設計判断

HAL を BOOT.SYS に組み込む段は、**PC-98 QEMU 無しでは検証不能**で、かつユーザの
カーネル設計に踏み込む判断が要るため、勝手に進めず相談したい:

1. **コンソールの所有権**: Boots は自前の console + BeUI で GDC を駆動(メニュー/Noct/
   Remacs が使用)。HAL は bsp-pc98/cons.c が別途テキストVRAMを直接叩く。
   案: HAL cons は早期ブート/パニック専用、boots_main 以降は Boots 側が画面を所有。
2. **kernel_entry の橋渡し**: cmain() 末尾が `kernel_entry()` を呼ぶ。
   案: `kernel_entry(){ crt_set_allocator(boots heap); i386_task_init(); boots_main(mbi); }`。
   boots_main のハンドオフは ADDR_BOOT_INFO(0x2000) 経由に変更。
3. **スケジューラのスタブ**: irq.c が `sched_link/sched_yield/sched_clock_handler` を参照。
   Phase B は単一タスク+tickのみなので、Phase D で kt の sched.c を載せるまで
   一時スタブ(tickはカウントのみ)で良いか。
4. **エントリ/リンク**: BOOT.SYS のエントリを locore `text_start` にし、stage1 は
   multiboot情報(EAX=0x2BADB002, EBX=mbi)を積んで PM ジャンプ。VMAは 0x80020000/
   0x80100000 へ higher-half リンク。→ 大きく、実機/QEMU 検証必須。

これらに方針をもらえれば Phase B のブート配線に進む。

---

## 追記2: QEMU 環境と HAL 起動グルー(960699c)

### QEMU 検証環境
`~/qemu-pc98/build/qemu-system-i386` をビルド済み(依存は apt 導入)。PC-98 BIOS ROM は
`~/qemu-pc98/roms/pc98bios/`(`make` で再生成、i486 ビルド)。ヘッドレス + monitor
socket + `screendump`→PNG でスクショ取得、`PC98_IDE_TRACE=1` で IDE ポートトレース、
gdb 接続も可能。現状 BIOS が POST 画面(`MEMORY ... OK / TAB: SETUP`)で停止し
ディスクブートのハンドオフに至らない。Boots コードではなくブートチェーン/BIOS 側の
可能性が高い。**この解析は保留**(ユーザ指示によりディスクブート検証はスタブ扱い)。

### HAL 起動グルー(コンパイル検証済み・未リンク)
ユーザ決定を反映:
- **crt_* → hal_***(hal_malloc/hal_printf/HAL_FATAL)に統一
- **fb 所有権**: `fb_set_active(1)` の間は cons が沈黙(`hal/i386/fb.c` + cons.c)。
  BeUI が表示を持つ間フラグを立てる
- **multiboot 撤去**: page.c は `bsp_mem_probe()`(BIOS ワークエリア)から総RAM取得、
  locore は multiboot ヘッダ/マジック検査を削除しハンドオフ物理アドレスを
  ADDR_BOOT_INFO に park(独自マジック B82H はカーネル側で検査)
- 初期直接マップ 128MB(ビットマップ4KB)、identity + higher-half 両マップ + Cirrus
  アパーチャ cache-disabled
- `kern/entry.S`(kernel_entry)、`kern/kmain.c`(heap を HAL アロケータに注入→boots_main)、
  `kern/sched-stub.c`(単一タスク用スタブ)
- 検証: `make ARCH=pc98 hal-pc98-compile kern-compile`(check 統合)全 PASS

### 残り: HAL を BOOT.SYS の実エントリにする flip(未着手・実機検証必須)
テストなしで動作パスを置換するのは危険なため未実施:
1. `stage2.ld` を higher-half VMA へ + HAL/kern を STAGE2_OBJS に + ENTRY を `text_start`
2. `patch-stage2.py` を契約 v3(物理 = vaddr & 0x7fffffff)
3. `bootsectors/pc98/stage1.S` を text_start へジャンプ、bit31 マスク、ハンドオフを EBX 渡し
4. **ゲートウェイ除去**: paged higher-half ではリアルモード BIOS 不可のため boots_main の
   `gw` 依存(キーボード/時計/表示リセット)を native 実装へ(= Phase B-4 と Phase C 大半が結合)

この flip はブートを壊すと即起動不能のため、QEMU ブート確認と一体で行うべき。

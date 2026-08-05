# Phase B: HAL 統合(higher-half 化と割り込み基盤)

前提: Phase A 完了。`docs/plan/00-overview.md` の地雷1〜3を再読すること。

## B-0. 目的と非目的

**目的**: BOOT.SYS を HAL(`hal/`)の上で動かす。
locore → hal_main → kernel_entry(=boots_main) の起動列、higher-half リンク、
IDT による例外表示、PIC/PIT の初期化まで。**タスクはまだ1本**(スケジューラは Phase D)。

**非目的**: ゲートウェイ削除(C)、プリエンプション(D)、fb の HAL 化(E)。
キーボード・時計・チェインブートは引き続きゲートウェイ(ブラケット付き)。

## B-1. bootsectors/ 再編(機械的リネーム)

16bit 世界と 32bit 世界を分離する(決定 D4):

```
platform/pc98/{disk-ipl.S, lba2.S, partition-pbr.S, boot2.S, fdd-ipl.S,
               stage1.S, chain-test.S, applet-test.S}
  → bootsectors/pc98/ へ git mv
```

- `platform/pc98/platform.mk` のパスを追随(`link-flat` 呼び出し等)
- `.S` 内の include は無いはずだが `grep -n "platform/pc98" bootsectors/pc98/*.S` で確認
- `platform/pc98/` に残るのは 32bit C の世界のみ(stage2.c, console.c,
  noct-*, timer.c, partition-pc98.c, abi.h, stage2.ld, stage2-entry.S)
- コミット: `Split 16-bit boot code into bootsectors/`

## B-2. HAL の自己完結化(決定 D9, D10)

アンカー: `grep -rn "kcrt\|malloc" hal/i386/*.c`

1. `hal/i386/lib.c`(新規): `hal_memcpy/hal_memset/hal_memcmp/hal_strlen` を実装し、
   HAL 内の `<sys/kcrt/kcrt.h>` include と `memset/memcpy` 呼び出しを置換
2. `hal/i386/task.c` の `malloc(sizeof(struct task_info))`:
   ```c
   /* hal.h に追加 */
   void hal_set_allocator(void *(*alloc)(size_t), void (*free)(void *));
   ```
   HAL 内部は登録されたフックを使う。未登録で task 生成に至ったら hal_panic
3. `assert/fatal` は `hal_panic()` に置換(メッセージは hal_cons 出力)
4. cons 属性(D10): `hal.h` に `void hal_cons_set_attr(int fg, int bg);` を追加。
   bsp-pc98: 属性 VRAM(0xA2000〜)への書き込み属性を状態として保持
   (PC-98 属性バイト: bit0=表示, bit5-7=色 等。`hal/i386/bsp-pc98/cons.c` の
   既存実装の流儀に合わせ、16色→PC-98 3bit 色への縮退表を書く)。
   bsp-pcat: VGA テキスト属性バイト(fg | bg<<4)そのまま
5. HAL のヘッダ参照(`<sys/types.h>`)は `hal/include/` 内に最小の
   `hal/types.h` を作って自己完結させる(uint8_t 等は stdint.h ベース)
6. コミット: `Make the HAL freestanding and add console attributes`

このステップは**ホストでコンパイルが通ること**を検証にできる:
```bash
gcc -m32 -march=i386 -ffreestanding -fsyntax-only -Ihal/include hal/i386/*.c \
    hal/i386/bsp-pc98/*.c   # (bsp は片方ずつ。エラーゼロ)
```

## B-3. アドレスレイアウトの整合(地雷1の解消)

`hal/i386/i386.h` を修正:

| 定数 | 旧 | 新 | 理由 |
|---|---|---|---|
| `ADDR_INIT_PT` | 0x00020000 | **0x00090000** | BOOT.SYS low セグメント(0x20000)と衝突。0x90000-0x9FFFF の 64KiB = PDT 4KiB + PT 15枚 → 15MiB マップ可 |

- `hal/i386/page.c` の初期マップ範囲が 15MiB 以内に収まるようにする
  (PC-98 の低位拡張は 14MiB まで + 15-16MB ホール。16MiB 以上の高位 RAM は
  初期マップに含めず、hal_page_map で後からマップする方針。
  page.c の現実装がどう組んでいるか読み、範囲定数を合わせる)
- boot params 0x80000-0x8FFFF、multiboot info 0x2000、IDT 0x1000 と衝突しないことを
  コメントとアサートで明記

## B-4. higher-half リンクと stage1 の対応

### B-4-1. リンカスクリプト(`platform/pc98/stage2.ld`)

- low: `. = 0x80020000;` high: `. = 0x80100000;`
- 各 PT_LOAD の **p_paddr を `AT()` で物理に指定するのではなく**、
  vaddr のまま出力し「物理 = vaddr & 0x7fffffff」を規約にする
  (kt bootsect と同じ。ld の出力で p_paddr==p_vaddr のままでよい)
- ASSERT を新アドレスに追随(low ≤ 0x80080000、high ≤ 0x80F00000)

### B-4-2. `scripts/patch-stage2.py` 契約 v3

- C5': low p_paddr==0x80020000, C6': high p_paddr==0x80100000
- 物理換算(& 0x7fffffff)での配置検査は従来どおり
- B98S ヘッダの entry フィールドには **物理アドレス(bit31 マスク済み)** を書く
  (stage1 が protected_entry でそのままジャンプできる値)

### B-4-3. `bootsectors/pc98/stage1.S`

1. ELF ローダの宛先計算に bit31 マスクを追加:
   `ph_paddr` を格納する箇所で `btrl $31, %eax`(kt サンプルの流儀)
2. **multiboot 疑似情報の構築**(locore の要求。アンカー:
   `grep -n "0x2badb002" hal/i386/locore.S`):
   - 90 バイトの multiboot_info を stage1 のデータ領域に用意
   - `flags = 1`(MEMORY)、`mem_lower = 639`、
     `mem_upper` = KB 単位の 1MiB 以上のメモリ量。16bit で BIOS ワークエリア
     0x401(128KB 単位の低位拡張)と 0x594(MB 単位の 16MiB 以上)から計算:
     `mem_upper = (*0x401) * 128 + (*0x594) * 1024`
   - PM エントリ直前に `EAX = 0x2BADB002`, `EBX = multiboot_info の物理アドレス`
   - 既存の handoff(EBX 渡し)と衝突する! → **handoff ポインタは
     multiboot_info の直後に置き、boot_device 情報は Boots 側が
     固定物理アドレス(現行 0x10000+stage2_handoff)から拾う方式に変更**。
     具体的には: stage2_handoff の物理アドレスを multiboot_info の
     未使用フィールド(boot_device, offset 12)に入れる。カーネル側は
     ADDR_BOOT_INFO(0x2000)+12 から読む
3. CHS ジオメトリ(決定 D3): 既に handoff デバイステーブルに入っている
   (INT 1Bh AH=84h の SENSE 結果)。変更不要なことを確認するのみ

### B-4-4. エントリの繋ぎ替え

- `platform/pc98/stage2-entry.S` は **B98S `.header` セクションだけ残し**、
  `_start32` を削除。エントリは `hal/i386/locore.S` の `_start`
  (リンカスクリプトの ENTRY を変更)
- locore は BSS をゼロしない(kt 流)。ゼロ埋めは `hal_main` 冒頭に
  `hal_bss_clear()` 相当を足すか、locore に low/high 2領域のクリアを追加する。
  **調査ポイント**: `hal/i386/locore.S` と `page.c` を読み、どの時点から
  高位アドレスが有効か確認してから位置を決める(paging 有効化前は
  物理アドレスでクリアする必要がある)
- `hal_main()` 末尾の `kernel_entry()` = `boots_main()` に接続。
  boots_main のシグネチャから handoff 引数を外し、ADDR_BOOT_INFO 経由の
  取得に変える(B-4-3)
- スタック: `ADDR_INIT_STACK`(0x3000)は 4KiB しかない。Boots の従来スタック
  0x8F000 相当の余裕が要るため、hal_main 突入後の早い段階で
  カーネル用スタック(low BSS 内に 32KiB 静的確保)へ切り替えるか、
  `SIZE_INIT_STACK` を拡大して場所を移す。**調査の上で決め、理由をコメントに残す**

## B-5. 割り込み基盤の有効化とゲートウェイのブラケット

1. `hal_main` の列(cons → page → int → task → irq → timer)をそのまま生かす。
   PIT tick(`CLOCK_HZ=100`)で `kernel_timer_handler()` が呼ばれるところまで確認
   (中身は当面 tick カウンタのインクリメントのみ)
2. **ブラケット**(地雷3): ゲートウェイ(`bootsectors/pc98/stage1.S` の
   gateway 系)の PM→RM 遷移部に以下を追加:
   - 入口: PIC を BIOS 配置(ベクタ 0x08/0x10)に再初期化、IDTR を実モード IVT
     (base 0, limit 0x3FF)に、割り込み許可
   - 出口: cli、PIC を HAL 配置(INT_IRQ_BASE=0xE0)に再初期化、IDTR を HAL の
     IDT に戻す(IDT の物理アドレス 0x1000 / limit は HAL 初期化時に stage1 の
     固定変数へ書き込んでおく — カーネル→stage1 の連絡は物理 0x10000 台の
     既知アドレスへの直接ストアでよい)
   - PIC 再初期化列は `hal/i386/bsp-pc98/pic.c` の ICW 列を写す
3. tick の取りこぼしはこの段階では許容(コメントで明記)

## B-6. 検証

```bash
make ARCH=pc98 all check    # 既知失敗以外 PASS、BOOT.SYS が契約 v3 を通過
readelf -l build/pc98/stage2.elf | grep LOAD   # VAddr 0x80020000 / 0x80100000
```
QEMU があれば: `test-hdd-boot.sh` `test-noct-repl.sh` `test-beui-menu.sh`
`test-beui-holoris.sh` が全部通ること(= 全機能が HAL 上で回帰)。
さらに例外表示の確認: 一時的に `int3` を仕込んだデバッグビルドでパニック画面が
出ること(確認後に削除)。

## B-7. コミット単位の目安

1. `Split 16-bit boot code into bootsectors/`
2. `Make the HAL freestanding and add console attributes`
3. `Relink BOOT.SYS higher-half and hand off through multiboot info`
4. `Boot Boots on the HAL with exceptions and a timer tick`

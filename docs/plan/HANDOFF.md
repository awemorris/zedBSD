# 引き継ぎ資料: Boots の HAL ブート統合(Phase B/C flip)

この文書だけを読めば、次の実装者が **HAL を BOOT.SYS の実エントリにする最後の統合**を
実装からテストまで完了できるように書いてある。上から順に実行すること。

- 作業ブランチ: **dev**(`git branch --show-current` で確認)。main には絶対コミットしない。
- コミットは dev に細かく積んでよい。メッセージは英語・現在形・末尾に
  `Co-Authored-By: Claude <モデル名> <noreply@anthropic.com>`。
- 各ステップ末尾の「確認」が通ってから次へ進む。通らなければ**止まって原因を直す**。
  チェックやアサートを緩めて通してはいけない。
- 行番号はこの文書を書いた時点のもの。**ズレていたら grep アンカーで探し直す**。
- 迷ったら設計を勝手に変えず、`docs/plan/STATUS.md` と本書の意図に従う。

---

## 0. まず全体像を 5 分で把握する

いま Boots は「リアルモード BIOS を間借りするブートローダ」として**動いている**。
このタスクは、それを「HAL(`hal/`)の上で動く 32bit カーネル」に切り替える最後の一歩。

### 現状のブート経路(動いている、変えるとブートが壊れる)
```
IPL(LBA0) → PBR → IO.SYS(=bootsectors/pc98/stage1.S, リアルモード)
  → stage1 が BOOT.SYS(2セグメントELF)を物理 0x20000/0x100000 にロード
  → stage1 が protected mode に入り、B98S v2 ヘッダの entry(物理アドレス)へジャンプ
  → platform/pc98/stage2-entry.S の _start32 → boots_main()
  → boots_main は「BIOSゲートウェイ」gw(リアルモードBIOS呼び出し)で
     ディスク以外(キーボード/時計/表示/チェイン)を処理
```
※ ディスクだけは Phase A で native IDE ドライバに置換済み。

### 目標のブート経路(このタスクで作る)
```
IPL → PBR → IO.SYS(stage1) → BOOT.SYS を同じくロード
  → stage1 が protected mode で locore(hal/i386/locore.S)の text_start へジャンプ
  → locore: GDT→BSSクリア→paging(higher-half)→TSS→IDT→cmain()
  → cmain(hal/i386/cmain.c): console→page→int→irq→timer→kernel_entry()
  → kernel_entry(kern/entry.S)→ boots_kernel_main(kern/kmain.c)
     → hal_set_allocator(heap) → boots_main()
  → boots_main は BIOSゲートウェイを一切使わない(native キーボード/時計/表示)
```

### なぜ B と C が一体なのか(最重要の理解)
higher-half + paging が有効なカーネルからは**リアルモード BIOS を呼べない**。
だから「HAL をエントリにする(B)」と「ゲートウェイ gw を消す(C)」は同時にやるしかない。
gw を残したまま HAL 化はできない。

---

## 1. 事前準備と現状確認

```bash
cd ~/boots
git branch --show-current            # dev であること
make ARCH=pc98 all                   # 現状ビルドが通ること(既存の動くBOOT.SYS)
make ARCH=pc98 hal-pc98-compile kern-compile   # HAL とグルーがコンパイルできること
```
3つとも成功すること。`check` は `stdio-fs-host-test` 等が 32bit リンクで失敗するが
**それは既知の環境問題(libc6-dev-i386 不在)**。他が通っていればよい。

### すでに用意されている部品(再実装しないこと)
| 部品 | 場所 | 役割 |
|---|---|---|
| HAL コア | `hal/i386/*.c,*.S` | IDT/paging/task/pmem/irq。**compile 済み** |
| PC-98 BSP | `hal/i386/bsp-pc98/{cons,pic,clock}.c` | コンソール/PIC/PIT |
| fb 所有権 | `hal/i386/fb.c` + `hal/include/sys/hal/fb.h` | `fb_set_active(1)` 中は cons 沈黙 |
| カーネルエントリ | `kern/entry.S` | locore→kernel_entry→boots_kernel_main |
| カーネル橋渡し | `kern/kmain.c` | heap を HAL allocator に注入 → boots_main |
| スケジューラスタブ | `kern/sched-stub.c` | 単一タスク用(tick はカウントのみ) |
| キーボード変換表 | `drivers/kbd-pc98-map.{h,c}` | scancode→正規化キー。**ホストテスト済み** |
| IDE ドライバ | `drivers/ide-pc98.c` | native ディスク(Phase A) |

---

## 2. QEMU 検証環境(テストに必須)

すでにビルド済み。使い方:

```bash
QEMU=~/qemu-pc98/build/qemu-system-i386
BIOS=~/qemu-pc98/roms/pc98bios          # pc98bios.bin 等がある

# HDD イメージを作る(既存スクリプト)
cd ~/boots && scripts/make-hdd-image.sh /tmp/hdd.img

# ヘッドレス起動 + monitor socket
$QEMU -M pc9821 -cpu 486 -m 8 -accel tcg -L $BIOS \
  -nic none -drive if=ide,bus=0,unit=0,format=raw,file=/tmp/hdd.img \
  -display none -serial none -monitor unix:/tmp/mon.sock,server,nowait -no-reboot &

sleep 8
# スクリーンショット(PNG化に netpbm の pnmtopng)
echo "screendump /tmp/shot.ppm" | socat - unix-connect:/tmp/mon.sock
pnmtopng /tmp/shot.ppm > /tmp/shot.png     # これを Read ツールで画像として見る
# メモリ/レジスタを見る
{ echo "x /8xb 0x20000"; sleep 1; echo "info registers"; sleep 1; } | socat - unix-connect:/tmp/mon.sock
pkill -f "file=/tmp/hdd.img"
```

- **CPU は `-cpu 486`**。BIOS ROM は i486 命令でビルドされている(386 だと BIOS が誤動作しうる)。
  ただし Boots 自身は `-march=i386` のまま(下限は i386)。
- `PC98_IDE_TRACE=1` を付けて起動すると IDE ポートアクセスが stderr に出る。
- **既知の未解決事項**: 現状 BIOS が POST 画面(`MEMORY ... OK / TAB: SETUP`)で止まり、
  ディスクブートのハンドオフに至らないことがある。これは Boots のコードではなく
  BIOS/ブートチェーン側の問題の可能性が高い。**Step 6 でこの切り分けを行う**。
- **重要**: QEMU の起動〜スクショはブラックボックスな「解析」ではなく、
  **自分が書いたコードの動作確認**として行うこと。1コマンドずつ短く、目的を明記する。

---

## 3. 実装ステップ

**順序厳守**。各ステップは独立してビルドが通る形にし、こまめにコミットする。

### Step 1: ゲートウェイ依存を native に置き換える(Phase C の本体)

**なぜ先にやるか**: これが終われば boots_main は BIOS 非依存になり、
その後の higher-half 化(Step 2-4)が「リンクとエントリの付け替え」だけになる。

対象は `platform/pc98/stage2.c` の `gw`/`call()` 経由の全呼び出し(アンカー:
`grep -n "call(BOOTS_BIOS_" platform/pc98/stage2.c`)。ディスク以外の残り:

| サービス | stage2.c の場所 | 置換先 |
|---|---|---|
| KEY_READ / KEY_POLL / KEY_STATE | `raw_key()` 496行, `poll()` 510行, `noct_key_is_down()` 562行 | 1-1: 8251A キーボードドライバ |
| CLOCK_SECOND | `clock_second()` 600行 | 1-2: RTC / tick |
| DISPLAY_RESET / DISPLAY_STOP | `beui_display_reset/stop()` 259-268行, 1680行, 2068行 | 1-3: GDC 直叩き |
| RETURN_MENU | `line()` 620行 | 1-4: 何もしない(no-op)化。下記参照 |
| PROBE_FIXED / REPROBE | 940行 | 1-5: IDE 再 probe |
| CHAIN_BOOT | 1458, 1834, 1849行 | 1-6: exit トランポリン(下記) |

#### 1-1. キーボード: `drivers/kbd-pc98.c`(新規)を書く
- ハード: 8251A。**データ 0x41 / ステータス 0x43**。IRQ 1。
  参照: `hal/i386/bsp-pc98/cons.c` の `get_keyboard_char()`(180行付近、
  `asm_inb(0x43) & 2` で RxRDY、`asm_inb(0x41)` でスキャンコード)。
  ステータスビットは `~/qemu-pc98/hw/input/pc98-kbd.c`:
  `KST_RXRDY = 0x02`, `KST_TXRDY = 0x01`。
- **変換は既存の `drivers/kbd-pc98-map.{h,c}` を使う**(再実装しない)。
  `struct boots_kbd_pc98` を1個持ち、IRQ かポーリングで生スキャンコードを
  `boots_kbd_pc98_feed()` に渡し、戻り値(正規化キー)をリングバッファに積む。
- 公開する関数(stage2.c が呼ぶ形。シグネチャは既存の noct_key_* に合わせる):
  ```c
  int  boots_kbd_pc98_read(void);      /* ブロッキング読み。正規化キーを返す */
  int  boots_kbd_pc98_poll(void);      /* キューにあれば>=0、無ければ<0 */
  int  boots_kbd_pc98_state(int key);  /* 正規化キーの押下: 1/0/-1 */
  void boots_kbd_pc98_drain(void);     /* type-ahead を捨てる */
  ```
  `boots_kbd_pc98_state` は `boots_kbd_pc98_is_down()`(map側)をそのまま使える。
- **Phase B は単一タスク**なので、まずは**割り込みを使わずポーリング**でよい
  (`boots_kbd_pc98_read` が 0x43 を回してキー入力を待つ)。IRQ 化は後の
  マルチタスク段でよい。ポーリングなら sched スタブと矛盾しない。
- stage2.c の差し替え:
  - `raw_key()`/`poll()`/`noct_key_is_down()`/`noct_key_drain()` の中身を
    上記4関数に置換。`boots_key_normalize_bios_ax()` はもう不要になる
    (map が正規化済みを返すため)。`key_to_scan()`(stage2.c 529行)も
    map 側に同等物があるので削除してよい。

#### 1-2. 時計: RTC + tick
- ミリ秒: **`kern/kmain.c` の `boots_kernel_ticks()`** が HAL の PIT tick
  (10ms/tick, CLOCK_HZ=100)を返す。BeUI clock HAL の `milliseconds` を
  「`boots_kernel_ticks() * 10`」にする(現状は `boots_pc98_timer_milliseconds`、
  stage2.c 2055行付近。これを置換)。**これで 26.7ms ポーリング制約が消える**。
  - `platform/pc98/timer.c`(ポーリング i8253)は不要になるので STAGE2_OBJS から外す。
- 秒(`clock_second`): µPD4990A RTC から読む。当面は簡略化してよい
  (tick ベースの秒 = `boots_kernel_ticks()/100 % 60`)。厳密な時刻が要るのは
  Remacs の表示程度なので、RTC 実装は後回しでも動く。**まず tick ベースで通す**。

#### 1-3. 表示リセット: GDC 直叩き
- `beui_display_reset/stop`(stage2.c 259-268行)は今 INT 18h 相当を gw で呼ぶ。
  native では GDC レジスタを直接叩く。**ただし** BeUI の GDC バックエンド
  (`noct/src/api/beui-pc98-gdc.c`)が既にモード制御を持っているので、
  display_reset/stop は「BeUI 側の enter/leave に委譲」または「テキストモードへ
  戻す最小限の GDC コマンド」にする。詳細は beui-pc98-gdc.c の enter/leave を読んで
  合わせること。**まずはテキスト表示に戻せれば十分**(メニューが見えればよい)。

#### 1-4. RETURN_MENU(stage2.c 620行, `line()` 内)
- これは「ESC でメニューへ戻る」ための BIOS 呼び出し。native では
  カーネル内制御フローで実現できるので、**呼び出しを削除**し、ESC は
  `line()` の呼び出し側が処理する(既にキー値 0x1b を見ているはず)。
  影響範囲が読めなければ **no-op 関数**にして先に進み、Step 5 の QEMU で挙動確認。

#### 1-5. PROBE_FIXED / REPROBE(stage2.c 940行付近)
- ディスク再スキャン。native では `boots_ide_pc98_init()` の再呼び出し +
  `boots_blkdev_reset()` で実現。該当箇所を差し替える。

#### 1-6. CHAIN_BOOT(stage2.c 1458, 1834, 1849行)= exit トランポリン
- 他 OS 起動。paged higher-half から**リアルモードへ降りる**必要がある。
  新規 `platform/pc98/exit-trampoline.S`(low セグメント配置)を書く:
  1. cli、全 IRQ マスク
  2. PIC を BIOS 配置(ベクタ 0x08/0x10)へ再初期化
  3. paging OFF(CR0.PG クリア)
  4. IDTR を実モード IVT(base 0, limit 0x3ff)へ
  5. リアルモードへ降りて対象ブートセクタへジャンプ
  - **これは最も難しく、最後に回してよい**。当面 CHAIN_BOOT は
    「未対応」エラー表示に置換し(メニューから他OS起動を一時的に無効化)、
    先に Linux ブートと Noct/BeUI を通す。チェインは別コミットで後追い。

**Step 1 の確認**:
```bash
make ARCH=pc98 all         # まだ gw ベースの旧エントリでリンク。ビルドが通ること
grep -n "call(BOOTS_BIOS_" platform/pc98/stage2.c   # 残りが CHAIN_BOOT だけ(一時許容)になっていく
```
この時点では**まだ旧 _start32 エントリのまま**でよい(boots_main が native 化されただけ)。
native キーボード/時計は Step 5 の QEMU で実際に確認する。

コミット例: `Replace the keyboard and clock gateway paths with native drivers`

---

### Step 2: リンカスクリプトを higher-half + HAL リンクにする

`platform/pc98/stage2.ld` を編集(現状は物理 0x20000/0x100000、`ENTRY(_start32)`)。

1. VMA を higher-half に:
   - low: `. = 0x80020000;` high: `. = 0x80100000;`
   - **物理アドレスは vaddr & 0x7fffffff**(bit31 を落とす)。ld の出力で
     p_paddr は p_vaddr のままでよい(patch スクリプトと stage1 が bit31 を落とす)。
2. `ENTRY(_start32)` → `ENTRY(text_start)`(locore のエントリ)。
3. **HAL とカーネルのオブジェクトを low セグメントに追加**。
   `.text.low` 等の各リスト(`grep -n "text.low\|\.header\|__low" platform/pc98/stage2.ld`)に:
   - locore/trap/dispatch/cmain/int/irq/task/univ/page/lib/fb(hal/i386)
   - bsp-pc98 の cons/pic/clock
   - kern の entry/kmain/sched-stub
   を **low に**入れる(カーネル本体は起動時に常駐する)。
   **注意**: HAL は `SYS_START=0x80000000` 前提のアドレスを使うので、
   higher-half VMA でリンクされる必要がある(このステップで満たされる)。
4. ASSERT を新アドレスに追随(low ≤ 0x80080000、high ≤ 0x80F00000)。

**Makefile(`platform/pc98/platform.mk`)**:
- `STAGE2_OBJS` に HAL/kern オブジェクトを追加(コンパイルルールは
  `hal-pc98-compile`/`kern-compile` のフラグを参考に、`$(BUILD)/hal/...`,
  `$(BUILD)/kern/...` を STAGE2_OBJS 用にビルドするルールを足す)。
  **HAL の C は freestanding フラグ + `-Ihal/include -Ihal/i386
  -DHAL_ARCH_I386 -DHAL_BOARD_PC98` が必要**(既存 `HAL_CC` 参照)。
- `stage2-entry.o` は**外す**(_start32 はもう使わない。B98S ヘッダは
  locore 側 or 別途 .header セクションで供給する必要がある → 下記注意)。

**注意(B98S ヘッダ)**: 現状 B98S ヘッダは stage2-entry.S の `.header` に置かれ、
stage1 と patch スクリプトが offset 512 にあることを前提にしている。
locore をエントリにすると .header の供給元が変わる。**`.header` セクションだけは
残す**(小さな別ファイル `platform/pc98/boot-header.S` に `.header` を切り出し、
low セグメント先頭に置く)。entry フィールドは patch が e_entry(物理)を書く。

**Step 2 の確認**:
```bash
make ARCH=pc98 build/pc98/stage2.elf
readelf -l build/pc98/stage2.elf | grep LOAD   # VAddr 0x80020000 / 0x80100000, 2本
readelf -h build/pc98/stage2.elf | grep Entry  # text_start のアドレス(low内)
nm build/pc98/stage2.elf | grep -E "text_start|kernel_entry|boots_main|cmain"  # 全部定義済み
```

コミット例: `Relink BOOT.SYS higher-half on the HAL`

---

### Step 3: patch-stage2.py を契約 v3 に

`scripts/patch-stage2.py`(現状 low=0x20000/high=0x100000 を検査)を更新:
- **物理配置検査を `vaddr & 0x7fffffff` で行う**: low の p_vaddr=0x80020000 →
  物理 0x20000、high の p_vaddr=0x80100000 → 物理 0x100000 を確認。
- C5/C6 を higher-half 値に更新。
- B98S ヘッダの entry フィールドには **e_entry の物理値(bit31 マスク済み)**を書く
  (stage1 がそこへジャンプするため。現状も物理を書いている)。
- 契約定数(`LOW_PADDR` 等)を新値に。

**Step 3 の確認**:
```bash
make ARCH=pc98 BOOT.SYS   # patch が通り、low/high サイズが表示されること
python3 - <<'EOF'
import struct
d=open('build/pc98/BOOT.SYS','rb').read()
assert d[:4]==b'\x7fELF' and d[512:516]==b'B98S'
fs,entry,ck=struct.unpack_from('<III',d,520)
assert entry & 0x80000000 == 0, "entry must be physical"
assert ck==sum(d[532:])&0xffffffff
print("v3 OK entry",hex(entry))
EOF
```

コミット例: `Update the BOOT.SYS contract for the higher-half layout`

---

### Step 4: stage1 を locore エントリへジャンプさせる

`bootsectors/pc98/stage1.S` の `enter_stage2:`/`protected_entry:`(アンカー:
`grep -n "enter_stage2:\|protected_entry:" bootsectors/pc98/stage1.S`)を編集。

現状(protected_entry, 末尾):
```asm
movl  $(0x10000 + stage2_handoff), %ebx
movl  0x0002000c, %eax        # B98S entry(物理)
jmp   *%eax                    # 直接ジャンプ = _start32
```
変更後:
- ジャンプ先は同じ(B98S entry = 物理の text_start)でよい。**locore は
  protected mode で入ることを期待している**(GDT/セグメントは stage1 が設定済み)。
- **EBX で渡すのは Boots ハンドオフの物理アドレス**(現状 `0x10000 + stage2_handoff`)。
  locore の `save_boot_info`(`grep -n "save_boot_info" hal/i386/locore.S`)は
  現状 `movl %ebx, (ADDR_BOOT_INFO)` で EBX を物理 0x2000 に置く。**これで整合する**。
- **ELF ロード時の bit31 マスク**: stage1 の ELF ローダは p_paddr をそのまま
  物理配置に使っている。`ph_paddr` を格納する箇所(457行 `movl %eax, ph_paddr(%di)`)
  か、配置先アドレスを計算する箇所(563行 `addl ph_paddr(%di), %eax`)の**どちらか
  一方**で bit31 を落とす。higher-half では p_paddr=0x80020000 なので
  `btrl $31, %eax`(または `andl $0x7fffffff, ...`)を1つ足す。**ここを忘れると
  0x80020000 に書こうとして即死する。最重要。** 手本は `~/kt/boot/bootsect/nec-pc98.S`
  の `btr eax, 31`。
- locore は自前で GDT を張り直す(`init_gdt`)。stage1 の GDT と二重だが、
  locore が上書きするので問題ない。ただし **stage1 が PE=1 で 32bit セグメントに
  いること**を locore は前提にする(現状の protected_entry がそうなっている)。

**追加のハンドオフ検査(ユーザ指示)**: multiboot は使わないが、独自マジック
`BOOTS_HANDOFF_MAGIC = 0x48323842 ("B82H")` の検査を locore or kmain 側に足す。
- 最小実装: `kern/kmain.c` の `boots_kernel_main` 冒頭で、渡された handoff の
  先頭4バイトが 0x48323842 でなければ `hal_fatal` 相当で停止。
  (boots_main 自身も既にこのマジックを見ているので、二重でも害はない。
  locore で見るなら `save_boot_info` の後に cmp を足す。)

**Step 4 の確認**: ビルドが通ること + Step 5 の QEMU 起動。
```bash
make ARCH=pc98 all
```

コミット例: `Enter the HAL from stage 1 and mask bit31 on load`

---

### Step 5: QEMU で起動を確認する(このタスクの検証の中心)

```bash
QEMU=~/qemu-pc98/build/qemu-system-i386
BIOS=~/qemu-pc98/roms/pc98bios
scripts/make-hdd-image.sh /tmp/hdd.img
$QEMU -M pc9821 -cpu 486 -m 8 -accel tcg -L $BIOS -nic none \
  -drive if=ide,bus=0,unit=0,format=raw,file=/tmp/hdd.img \
  -display none -serial none -monitor unix:/tmp/mon.sock,server,nowait -no-reboot &
sleep 10
echo "screendump /tmp/shot.ppm" | socat - unix-connect:/tmp/mon.sock
pnmtopng /tmp/shot.ppm > /tmp/shot.png    # Read ツールで見る
pkill -f "file=/tmp/hdd.img"
```

**期待**: HAL の起動ログ(`mem: N kb detected.` 等、hal_printf 由来)が出て、
その後 Boots のメニュー(`Boots/98` の BeUI 画面)が出る。

**デバッグの型**(gdb が使える):
```bash
$QEMU ... -s -S &     # -s: gdbstub tcp:1234, -S: 起動時停止
gdb -ex 'target remote :1234' -ex 'b *0x20014' -ex c    # 物理エントリで停止
# locore/cmain/boots_main のどこまで進むかをブレークポイントで追う
```
- 画面が真っ暗 → locore の paging か GDT でこけている。gdb で text_start から step。
- POST で止まる(`TAB: SETUP`)→ **Step 6 参照**(BIOS がディスクブートしていない)。
- HAL ログは出るがメニューが出ない → boots_main 内の native ドライバ(Step 1)を疑う。

各修正は「1コマンド起動 → スクショ/レジスタ確認 → 原因特定 → 直す」を短く繰り返す。

コミット例: `Fix <具体的な症状> in the HAL boot path`

---

### Step 6: BIOS がディスクブートに至らない件の切り分け(必要なら)

現象: QEMU の PC-98 BIOS が POST 画面で止まり、IDE にアクセスしない
(`PC98_IDE_TRACE=1` でアクセス0件)ことがある。これは**このタスク以前からの現象**で、
Boots のコードではなくブートチェーン/ディスクイメージ側の可能性が高い。

切り分け手順:
1. `PC98_IDE_TRACE=1` を付けて起動し、IDE アクセスが**1件でもあるか**確認。
   0件なら BIOS がブートを試みていない。
2. `scripts/make-hdd-image.sh` が作るディスクの**ジオメトリ(8ヘッド/17セクタ)**と
   **パーティションの bootable ビット(SID 0x91)**を確認
   (`~/qemu-pc98/hw/i386/pc98-mem.c` の 500-550行に BIOS がブート可能 IDE を
   判定するロジックがある: `ram[0x45d]` bit6、8ヘッドジオメトリ必須)。
3. LBA0 の IPL / LBA1 のパーティションテーブルが BIOS の期待通りか確認
   (`scripts/install-image.sh`)。
4. それでも BIOS がブートしないなら、**qemu-pc98 側の既知の制約**の可能性。
   `~/qemu-pc98` の git log / README でディスクブートの前提を確認する。
   最悪、`-drive` の指定や `-M pc9821` vs `pc9801`、`-cpu` を変えて試す。

**この切り分けは「自分のコードの検証のための環境確認」**として、目的を明記して
短いコマンドで行う。長時間の総当たりや、無関係なメモリダンプの大量取得はしない。

---

## 4. 完了の定義(Definition of Done)

- [ ] `grep -n "call(BOOTS_BIOS_" platform/pc98/stage2.c` が 0 件
      (CHAIN_BOOT を後回しにした場合はそれだけ残ってよいが、STATUS に明記)
- [ ] `readelf -l build/pc98/stage2.elf` が higher-half 2セグメント
- [ ] `make ARCH=pc98 all` 成功、`patch-stage2.py` v3 通過
- [ ] QEMU で HAL 起動ログ → Boots メニュー が出る(スクショを撮り Read で確認)
- [ ] メニューから **Noct/BeUI(例: Holoris や Remacs)が起動**する
      (`scripts/test-beui-*.sh` 相当を QEMU で手動確認)
- [ ] メニューから **Linux(vmlinux)がブート**する(該当 CFG で確認)。
      チェインブートは後回し可(その場合 STATUS に残タスクとして記載)
- [ ] `docs/plan/STATUS.md` を実態に更新
- [ ] 各ステップのコミットが dev に積まれている

---

## 5. やってはいけないこと

1. main にコミットしない。`noct/` submodule の中身・参照を変えない。
2. `NOCT_TARGET_PC98DOS`(noct 側の beui-pc98-*.c, api-beui-pc98dos.c)に触れない。
3. **動く BOOT.SYS を壊したまま放置しない**。各コミットでビルドは通すこと。
   higher-half 化の途中でブートが壊れるのは想定内だが、コミット単位では
   「ビルドは通る」状態を保つ。
4. QEMU を「解析ツール」として延々回さない。**自分のコードの動作確認**として、
   目的を明記した短いコマンドで使う。
5. 検証をすり抜けるために契約チェック(patch-stage2.py)や ASSERT を緩めない。
6. `~/kt` `~/qemu-pc98` `~/linux-pc98` は読み取り専用。変更しない。
7. IDE ドライバ(`drivers/ide-pc98.c`)は Phase A で実装済み。**再実装しない**。
   ハードウェアで未検証だが、ロジックは QEMU の pc98-ide モデルに合わせてある。

## 6. 参照(必要時に読む)
| 対象 | 場所 |
|---|---|
| 全体計画・確定判断 | `docs/plan/00-overview.md` |
| 現状ステータス・経緯 | `docs/plan/STATUS.md`(必読) |
| Phase B/C 元計画 | `docs/plan/02-hal-integration.md`, `03-gateway-removal.md` |
| HAL 公開契約 | `hal/include/hal/hal.h` |
| PC-98 キーボード実機挙動 | `~/qemu-pc98/hw/input/pc98-kbd.c` |
| PC-98 IDE 実機挙動 | `~/qemu-pc98/hw/ide/pc98-ide.c` |
| BIOS のディスクブート判定 | `~/qemu-pc98/hw/i386/pc98-mem.c` 500-550行 |
| ELF ローダの手本 | `~/kt/boot/bootsect/nec-pc98.S`(bit31 マスクの例) |

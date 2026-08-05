# Phase B/C 実行計画: HAL を BOOT.SYS の実エントリにする

この文書は `docs/plan/HANDOFF.md` を、実際の現行ソースに照合して実行単位まで
具体化したものである。実装者は本書を上から順に進めること。各チェックポイントの
確認が通るまで次へ進まない。特に、ビルドや契約検査を弱めて先へ進んではならない。

対象ブランチは `dev`。`main`、`noct/` submodule、`~/kt`、`~/qemu-pc98`、
`~/linux-pc98` は変更しない。後三者は参照専用である。

---

## 1. 完成状態

最終的な実行列を次に固定する。

```text
IPL -> PBR -> IO.SYS(stage1)
    -> BOOT.SYS の2個の PT_LOAD を物理 0x20000 / 0x100000 へ配置
    -> protected mode で物理 text_start へジャンプ
    -> locore: BSS -> GDT -> paging -> TSS -> IDT
    -> cmain: console -> pmem -> IDT -> PIC -> PIT -> sti
    -> kernel_entry -> boots_kernel_main -> boots_main
    -> native IDE / keyboard / PIT clock / GDC でメニューを実行
```

BOOT.SYS のリンク時 VMA は low `0x80020000`、high `0x80100000` とする。
物理アドレスは常に `VMA & 0x7fffffff` で求める。stage1 から locore へ渡す
`EBX` は従来どおり `struct boots_handoff` の物理アドレスであり、multiboot は使わない。

今回の必須範囲は、HAL起動、IDE、nativeキーボード、PIT時計、テキスト表示、
Noct/BeUI、vmlinux起動である。他OSへのチェインブートは明示的な「未対応」表示へ
置き換えてよい。未対応のまま完了する場合は `docs/plan/STATUS.md` に残す。

---

## 2. 現行コードを読んで判明した追加注意点

`HANDOFF.md` の手順だけでは見落としやすいので、以下を独立した修正項目として扱う。

1. `hal/i386/irq.c` のタイマーIRQは現在 `clock_handler()` と
   `sched_clock_handler()` だけを呼び、`kern/kmain.c` の
   `kernel_timer_handler()` を呼ばない。このままでは
   `boots_kernel_ticks()` は永久に0である。
2. `hal/i386/int.c:i386_int_init()` はPIC初期化前に `sti` する。stage1 は
   BIOS配置のPICのまま `cli` して渡すため、ここで割り込みを開くと新IDTと旧PIC配置が
   食い違う時間窓ができる。PIC/PITを初期化した後に一度だけ `sti` する。
3. v3ヘッダは物理entryを持つが、stage1の `elf_check_b98s` は現在それをELFの
   higher-half `e_entry` と比較する。`p_paddr` だけでなく、stage1が保存する
   `e_entry` にもbit31マスクが必要である。
4. 現在の `jump_linux()` はpaging有効状態から物理entryへ直接ジャンプする。
   identity mapのおかげで最初の命令には届いても、Linuxの従来契約はpaging OFFである。
   higher-halfコードからidentity aliasへ移ってからCR0.PGを落とすlow trampolineが必要。
5. `drivers/kbd-pc98-map.c` は正規化キーを返す一方、
   `core/noct-napi.c` と `platform/pc98/noct-target.c` はBIOS AXを再度正規化する。
   native化時に入力契約を「正規化済み」に揃えないと、例えば左キー `0x13b` が
   `';'` に化ける。
6. HALオブジェクトの現在のcompile-checkはbasenameで
   `build/pc98/hal/cons.o` 等へ出力する一時ループである。BOOT.SYSへリンクする際は
   ソース階層を保持した通常のmakeターゲットへ直し、同名オブジェクトや依存関係の
   取り違えを防ぐ。
7. `platform/pc98/timer.c` を先に外すと、旧 `_start32` 経路ではHAL PITが動いて
   いないため時計が停止する。また `boots_kernel_ticks()` もまだリンクされない。
   よって時計の切替はHALリンクと同じflipで行う。
8. GDCの640x400タイミングはstage1のBIOS呼び出しですでに設定されている。
   最初のnative実装はslave GDCのSTART/STOPを直接送る最小実装とし、タイミング列を
   不必要に再実装しない。QEMUで設定が失われる証拠が出た場合だけ、参照用BIOSの
   `~/qemu-pc98/roms/pc98bios/bios.S:int18_graph_area` に合わせて拡張する。

---

## 3. 作業開始時の基準値

毎セッション冒頭で次を実行し、ブランチとユーザ変更を確認する。

```bash
cd ~/boots
git branch --show-current
git status --short
make ARCH=pc98 all
make ARCH=pc98 hal-pc98-compile kern-compile
```

本書作成時の基準は次のとおり。

```text
branch: dev
stage2 entry: 0x20014
low:  filesz 0xaab4 / memsz 0xf9ec / 物理上限までの余裕 0x50614
high: filesz 0x44df0 / memsz 0x4abb8
HAL locore BSS: 151552 bytes
kern entry BSS: 32768 bytes
```

数値が変わっていても即異常ではないが、lowの最終物理endは必ず `0x80000` 以下にする。
ユーザの未コミット変更が対象ファイルにあれば、上書きせず差分を読んでから統合する。

---

## 4. チェックポイント0: ビルドルールとB98Sヘッダを先に整理する

この段階では実行経路を変えない。目的は、HALリンク時の巨大なMakefile差分を先に
機械的・検証可能な形へ分離することである。

### 4.1 B98Sヘッダの分離

新規 `platform/pc98/boot-header.S` を作り、現在
`platform/pc98/stage2-entry.S` にある `.header` と `__image_start` だけを移す。
`stage2-entry.S` には旧 `_start32` とBSSクリアを残す。

`STAGE2_OBJS` では、この時点では次の両方をリンクする。

```make
$(BUILD)/$(PC98)/boot-header.o
$(BUILD)/$(PC98)/stage2-entry.o
```

これによりエントリはまだ `_start32` のまま、B98Sヘッダ供給元だけが独立する。

### 4.2 HAL/kernオブジェクトを通常のmakeターゲットにする

`platform/pc98/platform.mk` で、ソースとオブジェクトを階層保持で列挙する。

```text
build/pc98/hal/i386/lib.o
build/pc98/hal/i386/irq.o
...
build/pc98/hal/i386/bsp-pc98/clock.o
build/pc98/kern/entry.o
build/pc98/kern/kmain.o
build/pc98/kern/sched-stub.o
```

要件:

- HAL C/asmは既存 `HAL_CC` 相当のフラグを使う。
- `.S` は `gcc ... -D_ASM_SRC_ -c` で前処理する。汎用の`as`ルールに流さない。
- `-MMD -MP` を付け、ヘッダ依存を生成する。
- `kern/kmain.c` は `BOOTS_KERN_CC`、`kern/sched-stub.c` と `kern/entry.S` は
  HALの型世界を使う既存フラグを維持する。
- `hal-pc98-compile` と `kern-compile` はシェルループで再コンパイルせず、上記の
  実オブジェクトへ依存するphony targetにする。
- この段階ではHAL/kernオブジェクトを `STAGE2_OBJS` へまだ追加しない。

### 4.3 確認

```bash
make ARCH=pc98 all
make ARCH=pc98 hal-pc98-compile kern-compile
readelf -h build/pc98/stage2.elf | grep 'Entry point'
python3 - <<'PY'
from pathlib import Path
d = Path('build/pc98/BOOT.SYS').read_bytes()
assert d[512:516] == b'B98S'
print('legacy header split: PASS')
PY
git diff --check
```

entryはまだ概ね `0x20014` で、legacy物理アドレスのままであること。

推奨コミット: `Separate the BOOT.SYS header and normalize HAL objects`

---

## 5. チェックポイント1: nativeポーリングキーボード

HAL flip前でも動作できる機能なので、単独コミットにする。

### 5.1 新規ドライバ

`drivers/kbd-pc98.h` と `drivers/kbd-pc98.c` を作る。ハードI/Oは次に固定する。

```text
data port:   0x41
status port: 0x43
RxRDY:       status & 0x02
break:       raw & 0x80
```

グローバルに1個の `struct boots_kbd_pc98` と、32個以上の正規化キーを保持する
リングバッファを持つ。ヒープは使わない。

公開契約:

```c
int      boots_kbd_pc98_read(void);
int      boots_kbd_pc98_poll(void);
int      boots_kbd_pc98_state(int key);
void     boots_kbd_pc98_drain(void);
unsigned boots_kbd_pc98_modifiers(void);
```

さらにNoct/Termへmodifier snapshotを失わず渡すため、ドライバ内部のevent契約を
`drivers/kbd-pc98.h` に定義する。

```c
#define BOOTS_KBD_EVENT_KEY_MASK  0x000001ffU
#define BOOTS_KBD_EVENT_SHIFT     0x00010000U
#define BOOTS_KBD_EVENT_CTRL      0x00020000U
#define BOOTS_KBD_EVENT_GRAPH     0x00040000U

int boots_kbd_pc98_read_event(void); /* consume */
int boots_kbd_pc98_poll_event(void); /* peek, -1 if empty */
```

リング要素は「正規化キー + そのmake時点のmodifier bits」とする。通常の
`read/poll` はevent APIを呼んで `KEY_MASK` だけを返す。これによりshellとBeUIは
単純な正規化キーを使い、RemacsはGraph-xをMeta-xへ変換できる。modifier単体はeventを
作らない。

各関数の意味を曖昧にしない。

- `read`: RxRDYをpumpし、キュー先頭を消費して正規化キーを返す。空ならpumpし続ける。
- `poll`: RxRDYを全てpumpし、先頭キーを**消費せず**返す。空なら `-1`。
- `state`: 先にpumpしてmake/breakを反映し、既存
  `boots_kbd_pc98_is_down()` の `1/0/-1` を返す。
- `drain`: 現在RxRDYの生コードを全てfeedして状態を更新した後、キーキューだけを空にする。
- `modifiers`: Shift/Ctrl/Graphの現在値をビットで返す。RemacsのGraph=Meta変換に使う。
- リングが満杯なら新規makeイベントを捨てる。breakはキューに積まないため、押下状態は
  必ずfeedで更新し、overflowしてもreleaseを失わない。

変換表は `drivers/kbd-pc98-map.{h,c}` だけを使う。別のASCII表を作らない。

### 5.2 stage2の入力契約を正規化済みにする

`platform/pc98/stage2.c` を次のように変える。

- `raw_key()` は `boots_kbd_pc98_read()` を呼ぶ。戻り値はすでに正規化済み。
- `key()` から `boots_key_normalize_bios_ax()` を外す。
- `poll()` は `boots_kbd_pc98_poll()`。
- `noct_key_read/poll` はmodifier付き `read_event/poll_event`、
  `is_down/drain` は通常のnative APIへ委譲する。
- stage2内の `key_to_scan()` を削除する。
- `pending_startup_key()` は現在どおり「pollで存在確認後、keyで消費」でよい。

Noct境界も同時に揃える。

- `core/noct-napi.c` のKeyboard.read/pollからBIOS AX再正規化を外し、eventの
  `BOOTS_KBD_EVENT_KEY_MASK` だけをNoctスクリプトへ返す。
- `platform/pc98/noct-target.c:translate_key()` はeventから正規化キーとmodifierを
  分離する。Graph bitは `NOCT_TERM_MOD_META` へ変換する。
  Ctrl文字はmapが返す `0x01..0x1f` の既存処理を保つ。
- `boots_key_normalize_bios_ax()` 自体はlegacy契約のテストや履歴のため、直ちに削除する
  必要はない。ただしnative入力パスからは呼ばない。
- `tests/noct-host-test.c` のBIOS AX形式モックを正規化キー形式へ更新する。

### 5.3 テスト

`tests/kbd-pc98-map-host-test.c` と `tests/noct-host-test.c` に最低限次を追加する。

- make -> ASCII -> breakでstateが `1 -> 0`
- Shift付き英字と記号
- Ctrl英字
- Graph modifierのmake/break状態
- 左右上下、ESC、Enter、F1/F10
- releaseとmodifier単体はキーを生成しない
- modifier付きeventをKeyboard.read/pollへ渡すと、スクリプトAPIはkey部分だけを返す
- 同じGraph付きeventをTerm adapterへ渡すとMeta付きキーになる

確認:

```bash
make ARCH=pc98 build/pc98/tests/kbd-pc98-map-host-test noct-host-test
build/pc98/tests/kbd-pc98-map-host-test
make ARCH=pc98 all
rg -n 'BOOTS_BIOS_KEY_|boots_key_normalize_bios_ax' platform/pc98/stage2.c
git diff --check
```

最終のgrepは0件であること。旧entryでビルドするため、ここではQEMU起動を壊していない。

推奨コミット: `Add the native polled PC-98 keyboard`

---

## 6. チェックポイント2: 時計以外のゲートウェイ呼び出しを除去する

この段階もentryは旧 `_start32` のまま。`CLOCK_SECOND` だけは旧経路を一時的に残す。

### 6.1 native GDC表示制御

新規 `platform/pc98/display-pc98.{h,c}` に次を置く。

- slave GDC command port `0xa2` へ `0x0d` でgraphics START
- slave GDC command port `0xa2` へ `0x0c` でgraphics STOP
- text復帰はgraphics STOP後、master GDC command port `0x62` へ `0x0d` でtext START
- slave commandはstatus `0xa0`、master commandはstatus `0x60` のFIFO full bit
  `0x02` がclearになるまで待つ。有限timeoutの書き方は既存
  `beui-pc98-gdc.c:gdc_command()` に合わせる
- timeoutは失敗を返し、無限loopにしない

stage1がAH=42h/CH=c0で640x400を設定済みなので、最初からSYNC/PITCH列を複製しない。
BeUIの `beui_display_reset/stop` はgraphics START/STOPへ委譲する。メニュー先頭、
AUTOEXEC終了後、shell復帰で使っていた `DISPLAY_RESET` は、同じ関数を流用せず
「graphics STOP + text START + `boots_console_reset()`」へ置き換える。

### 6.2 RETURN_MENU

`line()` のESC分岐からゲートウェイ呼び出しだけを削除する。`b[0]=0; return -1;` は
そのままにする。外側loopがstartup menuへ戻すため、別のgotoや再帰呼び出しを足さない。

### 6.3 disk probe

runtimeのデバイス記述子はstage1 handoffを正とする。native IDEのIDENTIFYはblkdevを
登録するが、BIOS論理geometryを新しく生成できないため、両者を混同しない。

- `ide_reported_drives()` と `scsi_reported_targets()` はBIOSワークエリアではなく、
  `discovered_devices[]` の `bios_id` からbitmapを作る。
- `probe_fixed_device()` は既知descriptorのindexを返し、未知unitをBIOS SENSEしない。
- 明示的な `probe-ide` では `boots_blkdev_reset()` の後に
  `boots_ide_pc98_init(devs, device_count)` を再実行する。
- `probe-scsi` は今回nativeドライバがないため、新規発見をせず明示的に未対応とする。
- `boots_main()` の初回IDE init前にも `boots_blkdev_reset()` を呼び、再入可能な順序へ揃える。

既知デバイスをstartup冒頭で `consider_automatic_device()` 済みであることを維持する。

### 6.4 chain boot

3箇所の `BOOTS_BIOS_CHAIN_BOOT` を共通の
`chain_boot_unsupported()` に置き換える。関数は
`"Chain boot is not available on the HAL yet.\n"` を表示して失敗を返す。

- shellの `boot` は `kernel_name` があれば従来どおりLinuxを優先する。
- kernel未選択時のbootだけが未対応表示になる。
- startupのPBR自動選択も未対応表示後にshellへ戻り、haltしない。

### 6.5 確認

```bash
make ARCH=pc98 all
rg -n 'call\(BOOTS_BIOS_' platform/pc98/stage2.c
git diff --check
```

この時点で許される出力は `CLOCK_SECOND` の1件だけ。`DISPLAY_*`、`RETURN_MENU`、
`PROBE_FIXED`、`CHAIN_BOOT` が残っていれば次へ進まない。

推奨コミット: `Replace the remaining non-clock BIOS services`

---

## 7. チェックポイント3: HAL側の起動前提を修正する

まだBOOT.SYSのentryは切り替えない。HAL/kern単体compileで修正を固める。

### 7.1 割り込み初期化順

次の順序に固定する。

```text
i386_int_init()  -> IDTを構築・loadするがIFは開かない
irq_init()       -> PICを0xe0/0xe8へ初期化し全IRQ mask
bsp_timer_init() -> PITを100Hzに設定しIRQ0だけunmask
asm_sti()        -> ここで初めてIFを開く
kernel_entry()
```

`hal/i386/int.c` から `asm_sti()` を外し、`hal/i386/cmain.c` で上記の最後に呼ぶ。
コメントも実際の順序へ更新する。

### 7.2 tick伝達

`hal/i386/irq.c` のIRQ_TIMER分岐を次の順にする。

```text
clock_handler()
kernel_timer_handler()
sched_clock_handler()
pic_send_eoi()
```

`kernel_timer_handler()` の宣言はHAL公開契約から得るか、循環includeを避ける最小宣言を
置く。timer ISRではallocationやconsole出力を行わない。

`kern/kmain.c` に次を用意する。

```c
uint64_t boots_kernel_ticks(void);
uint64_t boots_kernel_milliseconds(void *context);
```

millisecondsは `ticks * 10`。i386で64bit値をIRQと非同期に読むなら、32bit tickを使うか、
上位/下位が安定する読み方にしてtorn readを避ける。

### 7.3 handoff検査

`boots_kernel_main()` の先頭、allocator注入より前に次を検査する。

- pointer非NULL
- `magic == BOOTS_HANDOFF_MAGIC`
- `version == 1`
- `size >= sizeof(struct boots_handoff)`

失敗時は、すでにHAL consoleが初期化済みなので
`hal_fatal(__FILE__, __LINE__, "invalid Boots handoff")` で停止する。
`boots_main()` 側の詳細検査も残すが、`bios_gateway != 0` は要求しない。

### 7.4 Linux用paging-off trampoline

新規 `platform/pc98/exit-trampoline.S` に、vmlinux用32bit関数を作る。これは
リンカスクリプトで必ずlowへ置く。

処理順:

1. CからLinux physical entryと `BP_ADDR` を受け取る。
2. `cli`、master/slave PICを全mask。
3. paging有効中に、自分自身のidentity alias
   (`label - 0x80000000`) へ絶対jumpする。
4. identity alias上で `CR0.PG` をclearし、直後にjumpしてprefetchをflushする。
5. 以降stackを触らず、既存契約どおり `ESI=BP_ADDR`、`EBP=EDI=EBX=0` を設定する。
6. Linuxのphysical entryへjumpする。戻りは存在しない。

PICをBIOS vectorへ戻す処理、IVT load、real mode降下はvmlinuxには不要であり、今回の
未対応chain bootと混ぜない。

`stage2.c:jump_linux()` は最終flip時にこの関数を呼ぶ薄いwrapperへ変える。

### 7.5 確認

```bash
make ARCH=pc98 hal-pc98-compile kern-compile
nm -u build/pc98/hal/i386/*.o build/pc98/kern/*.o | sort -u
git diff --check
```

未定義シンボル一覧はオブジェクト単体なので存在してよいが、スペル違いや意図しない
libgcc helperが増えていないことを読む。

推奨コミット: `Fix HAL interrupt ordering and the kernel tick`

---

## 8. チェックポイント4: higher-half HAL flip

ここだけは複数ファイルを同時に変える。途中状態をコミットしない。以下を全部変更して
`make ARCH=pc98 all` が通った時点を1コミットにする。

### 8.1 STAGE2_OBJS

`platform/pc98/platform.mk`:

- `stage2-entry.o` を外し、`boot-header.o` は残す。
- HAL i386 core、PC-98 BSP、kernの全オブジェクトを加える。
- `drivers/kbd-pc98.o` と `drivers/kbd-pc98-map.o` が入っていることを確認する。
- `platform/pc98/timer.o` を外す。
- `platform/pc98/exit-trampoline.o` を加える。
- M9用 `stage2-m9-test.o` でも同じHAL closureを使う。

HAL/kernとして最低限必要なソースは現行の次の集合である。

```text
hal/i386/{lib,irq,page,univ,int,cmain,task,fb}.c
hal/i386/{locore,trap,dispatch}.S
hal/i386/bsp-pc98/{cons,pic,clock}.c
kern/{entry,kmain,sched-stub}
```

### 8.2 linker script

`platform/pc98/stage2.ld`:

- `ENTRY(text_start)`
- low開始 `0x80020000`
- high開始 `0x80100000`
- `.header` をlowの先頭、file offset 512に保つ
- HAL/kern/exit-trampolineの `.text/.rodata/.data/.bss` をすべてlowへ明示配置
- 既存のloader closureもlowに残す
- その他のNoct/BeUI/softfloatはhighのcatch-allへ送る
- `__low_bss_start/end` と `__high_bss_start/end` はlocoreがpaging前に物理aliasを
  clearするため、境界を壊さない
- ASSERTは `__low_end <= 0x80080000`、`__high_end <= 0x80f00000`

オブジェクト選択は曖昧な `*lib.o` ではなく、可能な限りbuild pathを含むselectorにする。
リンク後、orphan `.bss` や第三のLOAD segmentがないことを必ずreadelfで確認する。

### 8.3 patch-stage2.py契約v3

program headerから `p_offset,p_vaddr,p_paddr,p_filesz,p_memsz` を保持し、次を検査する。

- low `p_vaddr == p_paddr == 0x80020000`
- high `p_vaddr == p_paddr == 0x80100000`
- `physical = address & 0x7fffffff`
- low physical end `<= 0x80000`
- high physical end `< 0x0f00000`
- `e_entry` はlowの**virtual file range**内
- B98S entryへは `e_entry & 0x7fffffff` を書く

既存C1-C4、header offset 512、checksum契約は緩めない。docstringのC5/C6/C8もv3へ
更新する。

### 8.4 stage1のmask

`bootsectors/pc98/stage1.S:elf_parse_header` で次の2値を保存する直前に
`andl $0x7fffffff, %eax` を1回ずつ入れる。

- ELF headerの `e_entry` -> `elf_entry`
- program headerの `p_paddr` -> `ph_paddr`

これでB98S physical entryとの比較、unreal copy先、最終jumpがすべて物理値で揃う。
copy先計算側でも二重にmaskしない。

`protected_entry` は引き続きhandoff physical pointerをEBXへ入れ、B98S entry
`0x0002000c` を読んでjumpする。multiboot magicを追加しない。

### 8.5 stage2の最後の切替

`platform/pc98/stage2.c`:

- `platform/pc98/timer.h` includeを削除
- `clock_second()` は `(boots_kernel_ticks() / 100) % 60`
- BeUI millisecondsは `boots_kernel_milliseconds`
- 残った `CLOCK_SECOND` gateway callを削除
- `gw`、`rq`、`call()` を削除
- `boots_main()` のhandoff検査から `bios_gateway` 必須条件を削除し、代入も削除
- `jump_linux()` はlow exit trampolineへ移譲

### 8.6 framebuffer ownership proxy

HAL consoleはBeUI表示中に沈黙する必要がある。`noct/` を変更せず、
`beui_hal.display` の前にBoots側proxyを置く。

proxyは元の `struct noct_beui_display_hal` を保存し、同じ構造の全operation
(`enter/leave/poll_events/fill/line/pattern_fill/draw_image/
draw_image_pattern/flush`)をforwardする。display HALはcontextを全operationで共有するため、
enter/leaveだけcontextを差し替える実装は禁止する。

- proxy `enter`: `fb_set_active(1)` 後に元enter。失敗なら必ず `fb_set_active(0)`。
- proxy `leave`: 元leaveを先に実行し、その後 `fb_set_active(0)`。
- その他: 元contextと元callbackへそのままforward。
- glyph backendが参照する `beui_hal.display` はproxy化後の同じ場所を指すことを確認する。

### 8.7 flip直後の静的確認

```bash
make ARCH=pc98 all
make ARCH=pc98 hal-pc98-compile kern-compile
readelf -h build/pc98/stage2.elf | grep 'Entry point'
readelf -lW build/pc98/stage2.elf
nm build/pc98/stage2.elf | grep -E 'text_start|cmain|kernel_entry|boots_kernel_main|boots_main'
rg -n 'call\(BOOTS_BIOS_|\bgw\b|boots_pc98_timer' platform/pc98/stage2.c
python3 - <<'PY'
import struct
from pathlib import Path
d = Path('build/pc98/BOOT.SYS').read_bytes()
assert d[:4] == b'\x7fELF'
assert d[512:516] == b'B98S'
size, entry, checksum = struct.unpack_from('<III', d, 520)
assert size == len(d)
assert entry < 0x80000000
assert checksum == sum(d[532:]) & 0xffffffff
print('BOOT.SYS v3: PASS', hex(entry))
PY
git diff --check
```

期待値:

```text
LOAD 0: VAddr/PhysAddr 0x80020000
LOAD 1: VAddr/PhysAddr 0x80100000
LOAD数: 2
entry: 0x8002.... のtext_start
B98S entry: 0x0002.... の物理text_start
gateway grep: 0件
```

推奨コミット: `Relink BOOT.SYS higher-half on the HAL`

---

## 9. チェックポイント5: QEMU bring-up

QEMUは一度に1仮説だけ検証する。最初から全回帰スクリプトを回さない。

共通環境:

```bash
export QEMU=~/qemu-pc98/build/qemu-system-i386
export PC98_BIOS_DIR=~/qemu-pc98/roms/pc98bios
```

machineは `pc9821`、CPUは `486`、TCG、最初は8MiBとする。QEMUプロセスは起動時の
PIDを保存し、そのPIDだけを終了する。広い `pkill` は使わない。

### 9.1 起動ラダー

次の順に確認し、初めて失敗した境界だけを調べる。

1. BIOSがIDEへ1回以上アクセスする。
2. physical `0x20000` に `B98S` が現れる。
3. stage1がphysical `text_start` に到達する。
4. paging後にhigher-half `cmain` に到達する。
5. HAL consoleログが表示される。
6. Boots startup menuが表示され、1秒timeoutが進む。
7. ESC、文字、Enter、矢印、Shift、key-downが動く。
8. Noct/BeUIを開いて閉じ、テキストmenuへ戻る。
9. vmlinuxをロードし、paging-off trampolineからLinuxへ入る。

各段階の代表的な原因:

| 最初の失敗 | 調べる場所 |
|---|---|
| B98Sが来ない | BIOS/geometry/partition。BootsのHALコードを変更しない |
| text_start未到達 | patch v3 entry、stage1 e_entry/p_paddr mask |
| cmain未到達 | locore BSS/GDT/page table/far jump |
| HALログなし | HAL console VMA、BSS境界、page fault |
| menuなし | handoff検査、allocator、native IDE、text display restore |
| timeout停止 | IRQ初期化順、PIT mask、kernel_timer_handler |
| key停止 | 0x43 RxRDY、pump、キュー、正規化契約 |
| BeUI後に画面異常 | GDC START/STOP、display proxy、fb ownership |
| Linux直前で停止 | identity alias jump、CR0.PG clear後のstack使用 |

### 9.2 gdbの使い方

hard-codeした古いentry `0x20014` を使わない。ビルドしたELFからVMAを取り、bit31を
落としてphysical breakpointを計算する。

```bash
nm build/pc98/stage2.elf | grep ' text_start$'
```

QEMUを `-s -S` 付きで起動し、最初はphysical text_startで停止する。paging有効後は
higher-half ELF symbol (`cmain`, `kernel_entry`, `boots_main`) を使う。

一時的なport `0xe9` milestoneや故意の `int3` を使った場合、そのデバッグ変更は
動作確認後に必ず削除し、通常ビルドを再確認する。

### 9.3 BIOSがPOSTで止まる場合

`PC98_IDE_TRACE=1` でIDE accessが0件なら、HAL実装の失敗ではない。
`HANDOFF.md` Step 6に従い、8 heads / 17 sectors、LBA1のSID `0x91`、drive指定、
BIOS workareaのIDE判定を短いread-only確認で切り分ける。machine/CPUを無目的に
総当たりしない。

推奨コミット: 症状ごとに `Fix <specific symptom> in the HAL boot path`

---

## 10. チェックポイント6: 回帰テストと自動化

手動bring-upが通ってから既存QEMUテストを移行する。旧スクリプトには
`pc9801 -cpu 386` が残るものがあるので、最初から結果をHAL不具合と断定しない。

最低限の順序:

```bash
make ARCH=pc98 all
make ARCH=pc98 check                 # 32bit host libcの既知失敗は個別記録
QEMU="$QEMU" PC98_BIOS_DIR="$PC98_BIOS_DIR" scripts/test-hdd-boot.sh
QEMU="$QEMU" PC98_BIOS_DIR="$PC98_BIOS_DIR" scripts/test-noct-repl.sh
QEMU="$QEMU" PC98_BIOS_DIR="$PC98_BIOS_DIR" scripts/test-beui-input.sh
QEMU="$QEMU" PC98_BIOS_DIR="$PC98_BIOS_DIR" scripts/test-beui-menu.sh
QEMU="$QEMU" PC98_BIOS_DIR="$PC98_BIOS_DIR" scripts/test-beui-holoris.sh
QEMU="$QEMU" PC98_BIOS_DIR="$PC98_BIOS_DIR" scripts/test-autoexec-remacs.sh
```

テストスクリプトを変更する場合:

- HAL用標準profileは `pc9821 -cpu 486`。
- テスト自身が作った一時ディレクトリとQEMU PIDだけをcleanする。
- 単なるsleep成功ではなく、VRAM文字列、screendump、またはLinux側markerを検査する。
- `test-hdd-boot.sh` のphysical `0x20000 == B98S` 検査はv3でも有効。
- Linuxテストは「kernel load開始」ではなく、Linuxのconsole/login markerまで見る。

Noct/BeUIについて最低限確認する操作:

- Keyboard.read/pollでASCIIと左キーが正しい。
- Shift文字とCtrl-Cが正しい。
- BeUI `isKeyDown` がmake中1、break後0。
- Holorisまたは同等アプリが起動し、キーで終了できる。
- Remacsが起動し、Graph-xがMeta-xとして届く。
- BeUI終了後、HAL console ownershipが戻り、テキストmenuが見える。

---

## 11. 完了処理

`docs/plan/STATUS.md` を実態へ更新する。少なくとも次を記録する。

- 最終commit IDs
- readelfの2 LOADとentry
- BOOT.SYSのlow/high filesz、memsz、margin
- 実行したhost/QEMUテストと結果
- Linux起動をどのmarkerまで確認したか
- chain bootが未対応なら、その表示と残作業
- BIOS POST問題が残るなら、IDE access有無を含む環境切り分け結果

最終確認:

```bash
git status --short
git diff --check
rg -n 'call\(BOOTS_BIOS_' platform core drivers kern hal
readelf -lW build/pc98/stage2.elf
make ARCH=pc98 all
```

`call(BOOTS_BIOS_` は0件。意図しない変更、QEMU一時ファイル、スクリーンショット、
デバッグinstrumentationをコミットしない。

コミットは `dev` に細かく積み、英語・現在形のsubjectと、引き継ぎ指定の
`Co-Authored-By: Claude <モデル名> <noreply@anthropic.com>` trailerを付ける。

---

## 12. 中断条件

次の場合は推測で設計を変えず、そこで止めて証拠を残す。

- low segmentが `0x80080000` を越え、単純な配置調整では収まらない。
- ELFが2 LOADにならない、またはheaderがoffset 512から動く。
- stage1のphysical text_startに到達しないのに契約検査は通る。
- QEMUでIDE accessが0件のままPOSTから進まない。
- GDCの最小START/STOPで表示タイミングが失われ、BIOS相当の完全列が必要になる。
- vmlinuxが要求する物理load endがlocoreの128MiB identity mapを越える。
- ユーザ変更と同じ関数・linker sectionが競合し、意図を保存して統合できない。

報告には「最後に通ったチェックポイント」「失敗コマンド」「最初の異常境界」
「関係するreadelf/nm/レジスタの最小出力」を含める。大量の無関係なdumpは取らない。

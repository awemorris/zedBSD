# Phase C: native 入力/時計と BIOS ゲートウェイの削除

前提: Phase B 完了(HAL 上で全機能動作、ゲートウェイはブラケット方式で存続中)。

## C-0. 目的

ランタイムに残る BIOS 依存(キーボード・時計秒・チェインブート・再プローブ)を
native 実装に置き換え、**ゲートウェイ機構を stage1 から削除**する。
これで stage1 は「BOOT.SYS を配置して一度ジャンプしたら終わり」の純粋なローダになる。

## C-1. 残存 BIOS サービスの棚卸し(作業前に実行)

```bash
grep -n "BOOTS_BIOS_" platform/pc98/*.c core/*.c | grep -v abi.h
```
Phase A/B 完了時点で残っているはずのもの:
KEY_READ / KEY_POLL / KEY_STATE / CLOCK_SECOND / DISPLAY_RESET / DISPLAY_STOP /
REPROBE / CHAIN_BOOT / PROBE_FIXED。それぞれの置き換え先を以下に定める。

## C-2. キーボードドライバ `drivers/kbd-pc98.c`(新規)

参照: `~/qemu-pc98/hw/input/pc98-kbd.c`(グラウンドトゥルース)、
`~/linux-pc98/external/kernel/linux-7.1/drivers/input/keyboard/pc98kbd.c`

- ハード: 8251A USART。データ 0x41、ステータス/コマンド 0x43。IRQ 1
- スキャンコードは make/break(bit7=break)。**BIOS が今までやっていた
  「修飾キー追跡と文字への変換」を自前で持つ**:
  - 状態: Shift(0x70)/CTRL(0x74)/GRPH(0x73)/CAPS(0x71)/カナ(0x72)
  - スキャン→ASCII の変換表(通常/Shift の2面。カナ面は当面不要 —
    Remacs の SKK はローマ字入力)。PC-98 のスキャン配置は
    stage2.c 既存の `key_to_scan()`(逆方向の表)と pc98kbd.c を突き合わせて作る
  - 出力は現行ゲートウェイ互換の「正規化キー」
    (`boots_key_normalize_bios_ax` が返していた namespace = ASCII +
    NOCT_BEUI_KEY_*)。修飾情報の上位ビット詰め(GRPH等)は
    `platform/pc98/noct-target.c` の期待(アンカー:
    `grep -n "0x2a81\|bits 23:16" platform/pc98/noct-target.c tests/noct-host-test.c`)
    と互換にすること
- 割り込みハンドラはリングバッファに正規化前の生コードを積み、
  変換は読み出し側(カーネル)で行う。`key_read/key_poll/key_state` の
  カーネル内実装がゲートウェイ呼び出しを置き換える
- KEY_STATE(リアルタイム押下ビットマップ)は make/break 追跡で自前のビットマップを
  維持(BIOS ワークエリア 0x52A は**読まない** — BIOS はもう動いていない)

## C-3. 時計

- 秒(`CLOCK_SECOND` 置換): RTC µPD4990A から読む。
  参照: `~/qemu-pc98/hw/misc/pc98-sys.c` と linux-pc98 のRTC 読み出し。
  シリアルプロトコル(ポート 0x20 台)なので、tick ベースの秒カウンタを
  RTC で起動時に一度合わせる方式でよい(毎回シリアル読みしない)
- ミリ秒: Phase B の PIT tick(`hal_timer_get_tick` × 10ms)。
  `platform/pc98/timer.c`(ポーリング i8253)は**削除**し、BeUI clock HAL を
  tick 参照に差し替える(これで 26.7ms ポーリング制約が消える)

## C-4. exit トランポリン(vmlinux ブートとチェインブート)

新規 `platform/pc98/exit-trampoline.c` + 小さな .S。**low セグメント配置必須**。

共通処理(HAL を畳む):
1. cli、全 IRQ マスク
2. PIC を BIOS 配置(ベクタ 0x08/0x10)へ再初期化(ブラケットのコードを流用)
3. paging OFF(CR0.PG クリア、CR3 不問)、フラット GDT(既存の stage1 GDT でよい)
4. IDTR を実モード IVT(base 0, limit 0x3FF)へ

vmlinux ブート: 上記の後、boot_params(0x80000)を EBX 等の規約どおりに
セットして 32bit エントリへジャンプ(現行 `vmlinux_load` 後段の処理を
トランポリン経由に組み替え。paging OFF 後は物理アドレスで動くので、
トランポリン自体は物理==仮想が成り立つ low 領域で実行)。

チェインブート(D11): さらにリアルモードへ降りて他 OS のブートセクタへ
ジャンプする。現行 stage1 の CHAIN_BOOT 実装(RM 遷移列)を
**stage1 から bootsectors ではなくトランポリンへ移植**(16bit 部分は
.code16 でトランポリン .S 内に持つ。BIOS は生きているので INT 1Bh で
対象セクタを読む…のではなく、**チェイン対象セクタは事前に IDE ドライバで
0x7C00 相当へ読み込んでおき**、RM 降下後は即ジャンプのみにする)。

REPROBE / PROBE_FIXED(メニューのディスク再走査): IDE ドライバの再 probe に
置換。DISPLAY_RESET/STOP: すでに Phase B で HAL cons / GDC 制御が
カーネル側にあるので、INT 18h 相当のモード切替をカーネル内実装
(`hal/i386/bsp-pc98/cons.c` + GDC レジスタ直叩き)に置換。

## C-5. ゲートウェイ削除

1. `bootsectors/pc98/stage1.S` から gateway 系(`bios_gateway32`、
   `gateway_*`、ブラケット)を削除。handoff の `bios_gateway` フィールドは
   0 を書く(ABI 構造体は互換のため残し、コメントで retired と明記)
2. `platform/pc98/abi.h` の `enum boots_bios_service` にコメントで
   「Phase C で全サービス retired。番号は再利用しない」と明記(定義自体は残す)
3. stage2.c の `call()`/`gw` と全呼び出し箇所を削除
4. `tests/noct-host-test.c` のキーボード系モック(BIOS AX 形式)を
   新しい正規化キー供給の形に更新(アンカー: `grep -n "keyboard_bios" tests/noct-host-test.c`)
5. IO.SYS が大幅に縮むはず。サイズを報告に記録

## C-6. 検証

```bash
make ARCH=pc98 all check
grep -rn "call(BOOTS_BIOS" platform/ core/ | wc -l    # 0 であること
```
QEMU があれば全 QEMU テスト(hdd-boot / noct-repl / beui-menu / holoris /
remacs / term-japanese)+ Linux ブート(`BOOTS-LINUX.CFG` 系の verify)+
チェインブートの手動確認。**キーボードは必ず QEMU で対話確認が必要**
(pc98-kbd.c のエミュレーションと変換表の突き合わせ)。QEMU が無い環境では
変換表のホストテスト(スキャン列→正規化キー列の表駆動テスト)を書いて代替し、
「実機/QEMU 未検証」と明記して報告する。

## C-7. コミット単位の目安

1. `Add the native PC-98 keyboard driver`
2. `Drive the clock from the HAL tick and the RTC`
3. `Add the HAL exit trampoline for kernel and chain boot`
4. `Remove the BIOS gateway`

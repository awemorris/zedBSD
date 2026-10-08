<!-- awesome-plan project=zedbsd record=ws131-p013 -->

# ws131-p013: 旧 libkeiui の名前を kl_・KL_ に

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-085 は流さず、kl_ の改名は後続の p021〜p023 と多数の回帰で確かめ済み）（旧: in-progress（q673 の続き、P2 generation10、2026-10-04。Q1「While T2 runs, start ws131-p013 … on top of 488f96c, under the same delegation」。実装・build・host 試験済み、QEMU は後でまとめて））
Disposition: normal
Parent: [WS131](../ws.md)、計画の正本 [design.md](../design.md)
Queue: q673（p012 と同じ Queue の続き、Q1 の指示）
依存: p012 cleared
目安: 3〜4h（1 Queue）。実行者: Q1 が割り当てる（high）
所有 path: `userland/desktop/libkeiland/ui/`、`userland/desktop/include/`（新しい `keiland-ui.h`、互換の `keiui.h`、`keiland.h` の include）、`plan/ws131/tools/rename-map.py`、`plan/ws131/`

## 目的と結果

旧 libkeiui の 341 の公開の名前を [rename-map.md](../rename-map.md) のとおり `kl_`・`KL_` にする（2026-10-03 user の最終の規則）。`keiui.h` は旧名 → 新名の macro だけの互換の header、新しい宣言は `keiland/keiland-ui.h`（`keiland.h` が include）。app の source は無変更で build・動作する。

## 範囲

1. `keiui.h` の中身を `keiland-ui.h` に移して改名。`ui/` の source の公開名も改名（内部の `keiui_*` 55 個は保つ）。改名は rename-map の表から機械的に行い、手で改名しない。
2. 例外: `kui_version`・`KUI_VERSION` を除く（`kl_version` は p014 で）、`kui_edit_fn` → `kl_window_edit_fn`、`kui_keyboard_inset_fn` → `kl_window_keyboard_inset_fn`、`KUI_EDIT_*`・`KUI_KEYBOARD_INSET_*` は p014 で `KEILAND_*` と一本化するまで `KL_*` の定義を一つ置き、既存の `KEILAND_*` は p014 まで残す。
3. 互換の `keiui.h`: `#include <keiland.h>` と旧名の macro の列（tool で生成）、`KUI_VERSION 12U`。
4. FreeBSD の公開の header の表に `keiland-ui.h` を足す。

## 受け入れ

- 3 OS の build（zedBSD の amd64 は exit 0・自前の warning 0、`make keiland-linux` の gcc と clang、FreeBSD の native build と `native-build-audit.py`）と `plan/tools/keiland-os-boundary/check.sh` PASS。
- 利用者の source の diff が 0。`nm -D libkeiland.so` に `kui_` が無く、旧 `kui_*` の全てに `kl_*` がある。
- p012 と同じ回帰。

## 検証の方法

[zedbsd-commands.md](../../tools/keiland-linux/zedbsd-commands.md)、[keiland-linux/README.md](../../tools/keiland-linux/README.md)、[keiland-freebsd/README.md](../../tools/keiland-freebsd/README.md)。QEMU の console・serial の log で判定しない（不具合は gdbstub・monitor・QMP）。build は自分の `BUILD=build/ws131-pNNN/`、共有の `build/` を消さない。FreeBSD の起動の確認は判断 D11 の範囲（passthrough なし）。QEMU と実機の証拠を分ける。

## 衝突・危険・rollback

- 衝突: p012 と同じ。 順は Q1 が決める（design.md §7.2）。
- rollback: 前へ直すのを基本にし、後ろに同じ file の変更が無い時だけ統合の commit を revert する（design.md §7.4）。

## Resume

依存の Phase の cleared と main への統合、関係する判断の決定の後に、Q1 が Queue を作る。

## 実施（P2 generation10、2026-10-04、488f96c の上）

- 対応表: `python3 plan/ws131/tools/rename-map.py public > plan/ws131/rename-map.md` で今の header から作り直した（keiland.h 202・keiui.h 341、解決の要る衝突 0。keiland.h の数が 266 から減ったのは p003〜p011 で一部が既に `kl_`）。新しい `kl_`・`KL_` の名前は keiland.h の既存の `kl_`・`KL_`（`kl_settings_*`・`kl_system_*`・`KL_MONITOR_*` など）と重ならない（確かめた）。**注**: 変換の後の `keiui.h` は互換の macro だけなので、`public` を今流すと表は意味を失う。p014 の前に、`public` が `keiland-ui.h` を読むように直す。
- 変換は道具で一度だけ: `rename-map.py apply-ui`（同じ file に足した）が keiui.h の公開の名前の表（`kinds_of` と SPECIAL）から、
  1. `keiland/keiland-ui.h` = keiui.h の名前を改名したもの（guard `KEILAND_UI_H`、`kui_version`・`KUI_VERSION` の宣言と定義を除く。版の歴史の comment の `KUI_VERSION n` は残した）、
  2. `libkeiland/ui/` の source の改名と `#include <keiui.h>` → `<keiland.h>`（内部の `keiui_*`・`KEIUI_*` は保つ）、
  3. 互換の `keiui.h`（`#include <keiland.h>`、`KUI_VERSION 12U`、旧名 339 の `#define kui_X kl_X` の列、「generated … do not edit」の印）
  を書く。印のある keiui.h には二度と走らない。`rename-map.py check-ui` が互換の macro の行き先が全て keiland-ui.h にあり、keiland-ui.h の code に旧名が無いことを確かめる（PASS、339）。
- 例外の扱い: `kui_edit_fn` → `kl_window_edit_fn`、`kui_keyboard_inset_fn` → `kl_window_keyboard_inset_fn`、`KUI_EDIT_*`・`KUI_KEYBOARD_INSET_*` は keiland-ui.h に `KL_*` を一つずつ（`KEILAND_*` は p014 まで keiland.h に残る）。`kui_version` は除き、`ui/version.c` を消した（Makefile 3 本と host 試験の source の一覧からも）。
- `keiland.h` の最後で `#include <keiland-ui.h>`。`exports.py` は keiland.h と keiland-ui.h を読む。FreeBSD の公開の header の表に `keiland-ui.h`（`keiui.h` も残す）。zedBSD の sysroot は `userland/desktop/include` の全 file を拾うので toolchain の変更は要らない。
- host の試験: keiland.h を include の directory に写す・link する 12 本に keiland-ui.h も（ws081 の 6 本・ws089 の 2 本・ws100・ws131・ws128・files の host-build、ws090・ws102・keiui・textedit の 6 本）。内部の header だけを include していた `host-chooser.c`・`host-inset.c` に `#include <keiui.h>`。`host-draw.c` の `kui_version()` の確かめを除いた（12/12）。
- 古い試験の直し（main c21f9ab の素の tree でも失敗、WS134 の sysmon の追加の後）: `plan/ws131/tests/host-system.sh` に `wayland/sysmon.c`、`host-system.c` に monitor の backend の偽物（open は NULL、他は ENOTSUP）と `zwl_milliseconds`、能力の期待に `KL_SYSTEM_HAS_MONITOR`。

### 確かめ
- 利用者の source の diff 0（app・ime・kuidemo・files・monitor は無変更、`git status` で確かめた）。
- zedBSD: CI の clang 抜きの config の rootfs の build exit 0・自前の warning 0。`llvm-nm -D libkeiland.so` は 270 で `kui_` 0、旧 `kui_*`（kui_version を除く 150）の全てに `kl_*` がある、header の一覧と一致。
- Linux: gcc・clang exit 0・warning 0、`nm -D` が zedBSD と同じ、elf-check PASS（24）、makefile-sync PASS、header-check PASS（380）。FreeBSD は `make -n` まで（native build は T）。
- `keiland-os-boundary/check.sh` PASS、`rename-map.py check-ui` PASS。
- host の試験（全 PASS）: host-draw 12/12、host-input 63/63、host-widgets 94/94、host-chooser 85/85、scroll-bar-test、host-inset、textedit host-core 53/53、files host-default、imageview run-host、ws128 host-share、ws089 host-slot、ws100 host-audio 14/14、ws131 host-system、ws081 の motion・scroll・filestouch・termtouch・notestouch・browsertouch。
- QEMU（p012 と同じ組）・Linux の PNG・FreeBSD の native build は後でまとめて T に。

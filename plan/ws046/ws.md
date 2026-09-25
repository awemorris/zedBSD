<!-- awesome-plan project=zedbsd record=ws046 -->

# WS046: GNU 互換の make（autotools の出力を実行できる範囲）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG002
Related Milestones: MG001
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: なし
Resume point: p014（p011 の当て直し、BUG-033 の残り）。その後 p005
<!-- awesome-plan-current:end -->

## 目標

2026-09-24 ユーザー指示: 「GNU互換のmakeも必要です。GNU makeの全機能はいらないので、autotoolsの出力を実行できる程度には実装を進めましょう。
サブプロセスの並列度を取得するような機能はいらないです。」

`userland/base` に独自実装（Zlib）の make を作り、automake・autoconf が生成した Makefile（と libtool）で
package を guest で build・install できるようにする。今の base には make が無い。

## 範囲

- POSIX make（rule、suffix rule、macro、`$@ $< $* $? $^`、`-f -k -n -s -i -e -C`、`.PHONY`・`.SUFFIXES`・`.PRECIOUS`・`.DEFAULT`、include）。
- automake の出力が使う GNU の機能: `:=`・`+=`・`?=`、`ifeq`/`ifneq`/`ifdef`/`ifndef`/`else`/`endif`、`-include`、pattern rule（`%`）、
  関数（`$(wildcard)`・`$(patsubst)`・`$(subst)`・`$(filter)`・`$(filter-out)`・`$(dir)`・`$(notdir)`・`$(basename)`・`$(suffix)`・
  `$(addprefix)`・`$(addsuffix)`・`$(foreach)`・`$(if)`・`$(shell)`・`$(strip)`・`$(sort)`・`$(word)`・`$(words)`・`$(firstword)`・`$(call)`・`$(eval)` の要否）、
  target ごとの変数、`.SILENT`・`.NOTPARALLEL` の受け付け、`$(MAKE)` の再帰と `MAKEFLAGS`・`MAKELEVEL`、`V=0/1` の silent rule。
- **範囲外**: 並列 build（`-j`）と jobserver（サブプロセスの並列度を取得する機能、ユーザー指示で不要）、GNU make の全機能。

## 受け入れ

- expat・zlib・coreutils など、autotools で作られた package を guest で `./configure && make && make install` できる
  （WS042 の sh・WS043 の utility と合わせて）。
- host の GNU make と同じ Makefile で同じ結果になる差分試験。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws046-p001](phase001/phase.md) | 調査と設計: automake・autoconf・libtool の出力が実際に使う機能の一覧（expat・zlib・coreutils の Makefile を解析）、差分試験の土台 | cleared（q400-i01。automake の出力は POSIX make と入れ子の変数参照で動く。設計と差分試験 91 件） | — |
| [ws046-p002](phase002/phase.md) | POSIX make の核（読む、変数と展開、explicit・suffix・double-colon rule、特別な target、`include`、更新の算法、recipe の実行、option と `MAKEFLAGS`、再帰、内蔵の rule）。差分試験の `posix.sh` 45 件 | cleared（q401-i01。host・guest とも 45/45。automake の idiom も host で 15/15） | p001 |
| [ws046-p003](phase003/phase.md) | automake の idiom と GNU の機能（`:=`、条件、`define`、pattern rule、関数、target ごとの変数、`export`・`override`、VPATH、order-only、Makefile の作り直し）。差分試験の `automake.sh` 15 件と `gnu.sh` 31 件 | cleared（q403-i01。host・guest とも 91/91。host で expat の build・check・install が GNU make と同じ） | p002 |
| [ws046-p006](phase006/phase.md) | kernel の mkdir(2) が `.`・`..` に POSIX の errno を返す（[BUG-032](../bugs/BUG-032.md)。`mkdir -p ./.deps` が EINVAL で止まる） | cleared（q402-i01。mkdir の `.`・`..` は EEXIST、guest の automake の case 15/15） | p002 |
| [ws046-p004](phase004/phase.md) | 実 package の build（guest で expat などを configure・make・make install） | cleared（q404-i01。host で coreutils の build と check が通った。guest は configure と再帰の make・libtool・clang の連鎖まで。compile が遅すぎる件を BUG-033 として p007 に、残りの build を p008 に引き継いだ） | p003、p006、WS042、WS043 |
| [ws046-p007](phase007/phase.md) | guest の clang の遅さを測って直す（[BUG-033](../bugs/BUG-033.md)） | uncleared（q411-i01。主因は libc の allocator の `free()` が heap の全 block をたどること、と buffer cache の hash の偏り。両方直し、guest で `xmlparse.c` の compile 513 秒 → 5〜7 秒、expat の make 51 分以上 → 97 秒。512 MiB で受け入れの 2 項目が未達、残りを p009 へ。q405-i01 は中断） | p004 |
| [ws046-p009](phase009/phase.md) | guest の configure が遅い原因を調べて直す | cleared（q418-i01 uncleared: region の page の探索の list（2 乗）と reclaim の queue の探索を直した: file の fault 145 → 25 µs、configure 129 秒。q422-i01 cleared: HAL の rdmsr を `%gs:0` に（承認済み）: syscall 1340 → 450 ns、configure 96 秒。残り（file の fault の複写・object の入れ替え）は p012） | p007 |
| [ws046-p010](phase010/phase.md) | file の page の fault の複写を減らす（page cache の page をそのまま map、設計から） | cleared（q419-i01。MAP_PRIVATE の file の region も object の page を read-only で map し書き込みで COW） | p009 |
| [ws046-p011](phase011/phase.md) | 上の実装と試験 | uncleared（q420-i01。compile と link は速くなったが configure が 128 → 270 秒と遅くなり make が失敗、戻した。object の寿命の費用） | p010 |
| [ws046-p012](phase012/phase.md) | private の file の mapping の page cache の共有の設計を直す（object の寿命、同期の walk） | cleared（q430-i01。LRU の追い出し、mapping の object の cache 化、clean な walk の省略、`KERN_SYSTEM_DROP_CACHES`。link 0.51〜0.68 → 0.36〜0.38 秒、file の fault 25 → 15.3 µs/page、configure 91 秒。p011 の当て直しは p014） | p011 |
| [ws046-p008](phase008/phase.md) | 実 package（expat・zlib・coreutils） | uncleared（q421-i01。expat・zlib は guest で最後まで。coreutils は cross build（ユーザー判断）: libc の header の誤り 2 つ（locale.h の循環、WINT の限界）を直したが、libc に mount の一覧の API が無く止まった → p013） | p007 の修正 1・2 |
| [ws046-p013](phase013/phase.md) | libc に mount の一覧の API（getmntinfo など）を足す | cleared（q424-i01。`<mntent.h>` の getmntent 系を kernel の mount query の上に実装。coreutils の cross build を通すために libc に `<elf.h>`・`<stdio_ext.h>`・`<utime.h>`・`fseeko`・spawn の `_np`・errno 14 個・`statvfs.f_basetype` も足し、configure・make・install が通り guest で `df`・`stat`・`ls` などが動く。`df` の既定の出力の欠けは BUG-047（kernel の `st_dev` 0）） | — |
| [ws046-p014](phase014/phase.md) | private の file の mapping で page cache の page を直接 map する（p011 の当て直し、p012 の設計 5） | uncleared（q434-i01。private の file の mapping で cache の page を map、書き込みで COW。file fault 8.4 → 3.2 µs、`cc t.c -o t` 0.35 → 0.25 秒、configure（tmpfs）35 → 30〜33 秒。expat の make は status 0、runtests 4932/4932。`make check` だけ bash が無く status 2: 判断待ち） | p012 |
| ws046-p005 | 規約の全文確認と回帰 | planning | p004 |

2026-09-24（ws046-p001）: 調べた結果、automake の出力と zlib は POSIX make と入れ子の変数参照で動き、GNU の関数・条件は coreutils の
`GNUmakefile`（maintainer 用）だけが使う。`GNUmakefile` は読まない、`-j` は受け付けて無視、GNU make だと名乗らない、と決めた（p001 の設計）。
p002・p003 の範囲を差分試験の case 群で決めた。範囲外は Future Work F-007。

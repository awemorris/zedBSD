<!-- awesome-plan project=zedbsd record=ws046p008 -->

# ws046-p008: 実 package を guest で最後まで build する

Phase ID: `ws046-p008`
Parent: [WS046](../ws.md)
Status: uncleared（q421-i01、2026-09-25。coreutils の cross build は libc の header の誤り 2 つを直して進んだが、libc に mount の一覧の API が無く止まった）
Queue: q417（q417-i01）。q412-i01 は uncleared

## きっかけ

[ws046-p004](../phase004/phase.md) の残り。guest の compile が遅すぎた件（BUG-033）は [ws046-p007](../phase007/phase.md) で主因を直し、guest で expat の `make` が 97 秒になった。

## 対象と手順（guest: amd64、QEMU・KVM、harness の既定の 512 MiB）

1. **expat 2.8.5**: `./configure --build=x86_64-unknown-zedbsd`、`make`、`make check`、`make install DESTDIR=...`。
   expat の試験の driver の包み（`test-driver-wrapper.sh`）は bash で書かれていて（配列、`[[`、`type -p`）guest に bash が無いので、
   `make check LOG_DRIVER='$(SHELL) $(top_srcdir)/conftools/test-driver'` で automake の POSIX の `test-driver` を直に使う。
   包みは引数を並べ直して `run.sh`（mingw 以外では `exec "$@"`）を挟むだけなので、走る試験は同じ。
2. **zlib 1.3.2**（手書きの configure と Makefile）: configure、`make`、`make check`、`make install DESTDIR=...`。
3. **coreutils 9.12**: configure、`make`、`make install DESTDIR=...`。`make check` は走らせて結果を分類して記録する（perl の要る試験は SKIP になる）。

install された file の一覧を host（GNU make）の同じ package のものと比べる。

## 受け入れ

- expat と zlib が guest で configure・build・check・install まで status 0。install の file の一覧が host と同じか、違いが system の違いで説明できる。
- coreutils が guest で configure・build・install まで status 0。check の結果を分類して記録する。
- 止まったら、make の不具合は直し（WS046 の範囲）、他（sh・utility・kernel・libc・compiler）は分類して Bug に記録する。
- make の差分試験 91/91 が下がらない（make を変えた場合）。

## 調べる範囲

1〜3 を 1 回ずつ。make 以外の不具合を直すのはこの Phase の外（記録して、要るなら別の Phase）。

## 結果（q412-i01、2026-09-24）

guest の既定の disk（overlay の上層の `data.img`）は 32 MiB しかなく、coreutils の build が入らない。共有の `build/data.img` は変えず、
build の道具（`tools/build/make-data-image.noct --size-mib 1024`）で 1 GiB の UFS の image（`build/ws046-p008-work.img`）を作り、
harness の `--qemu-extra` で xHCI の port 4 に 2 台目の usb-storage としてつなぎ、guest で `mount -t ufs /dev/sdb /work` して作業場にした
（guest の `mkfs -t ufs /dev/sdb` は EINVAL: `mkfs` は image の file を作る道具で、device には書かない）。

### expat 2.8.5（guest 512 MiB、`/work`）

| 手順 | 結果 |
| --- | --- |
| `./configure --build=x86_64-unknown-zedbsd` | status 0、225 秒 |
| `make` | status 0、142 秒 |
| `make check LOG_DRIVER='$(SHELL) $(top_srcdir)/conftools/test-driver'` | status 2: 4932 の check のうち 12 が失敗、全て `test_buffer_can_grow_to_max`（chunksize と deferral の 12 の組） |
| 同じ check を guest 2 GiB で | **status 0、4932/4932**。この試験は 1 GiB（`INT_MAX / 2`）の buffer を確保する。512 MiB の guest では確保できない（zedBSD は overcommit しない）。upstream も 32 bit の mingw で同じ理由の回避を持つ。system の違い |
| `make install DESTDIR=/work/expat-dest` | status 0。host（GNU make）の一覧（31）との差は `libexpat.so`・`.so.1`・`.so.1.12.5` の 3 つだけ: libtool が zedbsd を知らず（`dynamic linker characteristics... no`）共有 library を作らない（F-008） |

### zlib 1.3.2（guest 512 MiB、`/work`）

| 手順 | 結果 |
| --- | --- |
| `./configure`、`make`、`make check`、`make install DESTDIR=...` | 全て status 0（configure 18 秒、make 29 秒）、`*** zlib test OK ***` |
| install の一覧 | host（GNU make）との差は `libz.so`・`.so.1`・`.so.1.3.2` だけ。configure が「No shared library support」: `uname` の `zedBSD` は `*BSD` に当たり `cc -shared -Wl,...,--version-script,zlib.map` を試すが、guest の ld.lld が version script の未定義の symbol を error にする（lld の `--no-undefined-version` の既定。host は GNU ld）。F-009 |
| `LDFLAGS=-Wl,--undefined-version` で同じ手順 | 共有と static の両方を作り、`*** zlib test OK ***` と `*** zlib shared test OK ***`、**install の一覧が host と一致** |

### coreutils 9.12（guest 512 MiB）: 展開で止まった

1. GNU tar の `--format=pax` の archive を guest の `pax -r` が読めない（`unsupported archive member type x`、[BUG-037](../../bugs/BUG-037.md)）。`--format=ustar` で作り直した（名前は全て ustar に収まった）。
2. 1 GiB の作業の UFS に展開すると、`m4/gethostname.m4`（`m4/` の 370 番目）で `No space left on device`。空きは 917 MiB。
   inode を 16384 に増やした image（`zedimage-host ufs ... --inodes=16384`、1 GiB で受け付ける最大）でも同じ所で止まった。
   空の directory に短い名前の file を作ると 670 番目で ENOSPC、directory は 8192 byte のまま。
   **UFS の directory が 1 block（8 KiB）を超えて育たない**（`dir_add()` の設計の限界、[BUG-038](../../bugs/BUG-038.md)）。`m4/` は 492 の file を持つ。
3. 起動の 1 回で usb-net（CDC-ECM）の attach が error 3 で失敗し SSH できなかった（[BUG-036](../../bugs/BUG-036.md)、間欠、再起動で直った）。

make 以外の不具合を直すのはこの Phase の外なので、BUG-038 を直す WS054 を立てて uncleared にした。coreutils は WS054 の後に再開する。

### 判定

| 受け入れ | 結果 |
| --- | --- |
| expat と zlib が configure・build・check・install まで status 0、install の一覧が host と同じか system の違いで説明できる | 達（expat の check は 512 MiB で 1 GiB の確保の試験が落ち、2 GiB で 4932/4932。共有 library の差は libtool（F-008）と ld.lld の既定（F-009）で説明でき、zlib は `--undefined-version` で host と一致） |
| coreutils が configure・build・install まで status 0 | **未達**: source の展開で止まった（BUG-038。BUG-037 は ustar で避けた） |
| make の差分試験 | make を変えていない |

再開の条件: WS054（BUG-038）の後に、同じ手順（`/work` の作業の disk、ustar の archive）で coreutils から。

## q417-i01（2026-09-25、uncleared: 範囲の変更）

WS054 で展開の限界が無くなったので、guest で coreutils を再開した。`config.sub` が `zedbsd` を知らなかったので `build-aux/config.sub` に足した（expat の `conftools/config.sub` と同じ。host の build の記録は gnulib の新しい `config.sub` を確かめていなかった）。
configure は約 20 分で 951 の check（1 check 約 1.3 秒。Linux では数十 ms）で、ユーザーの指示で止めた。

2026-09-25 ユーザー判断: 「coreutilsはクロスビルドできればそれでいいです。それよりも、ゲストでconfigureすると遅すぎるようです。その原因を探って修正する必要があります。漫然と長時間configureやビルドを実行しないでください。妥当なビルド時間であるか検討して、遅すぎるなら原因を調査して修正しましょう。」

- 受け入れの coreutils は「guest で configure・build・install」から「host で zedBSD 向けに cross build（configure・build・install）」に変わった。残りはそれだけ。
- guest の configure の遅さは ws046-p009 を書き直して扱う。

## q421-i01（2026-09-25、uncleared）: coreutils の cross build

host（Linux）で zedBSD 向けに cross build した: `CC="build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot=build/amd64/sysroot"`、
`LDFLAGS="-L build/ws043-guest/dynamic -Wl,-rpath-link,..."`（sysroot の static の `libc.a` は `strtod`・`strtold`・`__syscall6`・`__signal_restorer` が未定義で使えない。userland は `libc.so` で link する）、
`./configure --host=x86_64-unknown-zedbsd --build=x86_64-pc-linux-gnu --disable-nls`（`build-aux/config.sub` に `zedbsd*` を足した）。configure は host で 86 秒、status 0。

make で見つけて直した libc の header の誤り（`include/libc/`、全ての userland に効く）:

1. **`<locale.h>` の include の循環**: `locale.h` が `locale_t` を定義する前に `<stdint.h>` を読み、gnulib の置き換えの `stdint.h` → `inttypes.h` → `wchar.h` → `stdio.h` が
   まだ無い `locale_t` を使って error。`locale_t` の typedef を include の前に移した。
2. **`WINT_MIN`・`WINT_MAX` が `wint_t` と合わない**: `<wchar.h>` は `wint_t` を `uint32_t`（符号なし）と定義するのに、`<stdint.h>` は compiler の `__WINT_MIN__`・`__WINT_MAX__`（`int`）を使っていた。
   gnulib の「stdint.h は C99 に合うか」の検査が `check_WINT` で落ち、`stdint.h` を置き換え、それが上の循環を起こした。限界を型に合わせた（`0U`・`UINT32_MAX`）。
   `wint_t` を compiler の `int` に合わせる直し方もあるが、libc++ の C++ の名前（mangling）が変わるので選ばなかった。compiler の `__WINT_TYPE__`（`int`）と libc の `wint_t`（`uint32_t`）の食い違いは残る（書式の警告に出うる）。**確認事項**（どちらに揃えるか）。

止まった所: `lib/mountlist.c: error: "Please port gnulib mountlist.c to your platform!"`。zedBSD の libc に mount の一覧の API（`getmntinfo`・`getfsstat`・`getmntent`）が無い
（`mount` は一覧を出せるので kernel の情報はある）。libc に `getmntinfo`（または `getmntent`）を足すのは新しい機能で、別の Phase が要る（ws046-p013 として計画）。

回帰（header の変更の後）: 4 platform の build（warning 0）と boot（amd64・rpi4・pcat で login prompt、pc98 は login と `uname -a`）、guest の make の差分試験 91/91・対話試験 41/41
（[evidence/](evidence/)）。guest の sh の差分試験は 1388/1425（落ちる集合は前と同じ）。


## 追記（2026-09-25、q424）

coreutils の cross build（configure・make・install）は ws046-p013 で通った（libc の追加は p013 の記録）。guest で `df`・`stat`・`ls` などが動く。この Phase の残りだった coreutils は p013 の結果で達成。

<!-- awesome-plan project=zedbsd record=ws046p001 -->

# ws046-p001: GNU 互換の make の調査と設計

Phase ID: `ws046-p001`
Parent: [WS046](../ws.md)
Status: cleared（q400-i01、2026-09-24）
Queue: q400（q400-i01）

## 目的

automake・autoconf・libtool が生成する Makefile（expat 2.8.5、coreutils 9.12）と、手書きに近い zlib 1.3.2 の Makefile が実際に使う
make の機能を数え、`userland/base/make` の設計（file の分け方、データ構造、変数の展開の規則、rule の探し方、command の実行）と
差分試験（host の GNU make 4.4.1 と比べる）の土台を作る。

## 方法

1. 3 つの package を host で configure し、生成された Makefile を解析する: 指令（`include`・条件・`define`・`export`・`vpath` ほか）、
   代入の種類、関数、自動変数、特別な target、pattern rule と suffix rule、target ごとの変数、再帰の `$(MAKE)`、`MAKEFLAGS` の使い方。
2. GNU make で `make -n`・`make`・`make install DESTDIR=...`・`make check` を走らせ、どの rule と機能が実際に通るかを確かめる。
3. 結果から設計を書く。範囲外（`-j`、jobserver）は入れない。
4. 差分試験の土台: `plan/ws046/tests/make-diff.py`（case の Makefile を GNU make と我々の make で走らせ、stdout・stderr の要点・status・作られた file を比べる）と、最初の case 群。
   我々の make はまだ無いので、この Phase では GNU make だけで case が期待どおりに動くことを確かめる。

## 受け入れ

- 機能の一覧（どの package のどこで使うか、必須か）と、その根拠の数え方が phase.md にある。
- 設計が phase.md にあり、p002（POSIX make の核）と p003（GNU の機能）の範囲がそれで決まる。人間の判断が要る点があれば書く。
- `make-diff.py` が GNU make で全 case を通す。

## 調べたこと（q400-i01、2026-09-24）

host（dash と GNU make 4.4.1）で expat 2.8.5（`--disable-dependency-tracking` と既定の 2 通り）、zlib 1.3.2、coreutils 9.12 を configure し、
生成された Makefile を `plan/ws046/tests/make-features.py` で数えた（`evidence/features.txt`）。expat は GNU make で `make`・`make check`・
`make install DESTDIR=...` が全て status 0。

| 機能 | expat（automake） | coreutils の Makefile（automake） | zlib（手書き） | coreutils の GNUmakefile・maint.mk | 要否 |
| --- | --- | --- | --- | --- | --- |
| `=` の代入 | 1761 | 3775 | 48 | 多数 | 必須 |
| `:=`・`?=`・`+=`・条件の指令・`define`・関数（`$(shell)`・`$(filter)`・`$(call)` ほか） | 0 | 0 | 0 | あり | GNUmakefile だけ（下の決定） |
| `include`（`./$(DEPDIR)/x.Plo # am--include-marker`） | 41（依存の追跡あり） | 依存の追跡ありで同様 | 0 | あり | 必須 |
| 入れ子の変数参照（silent rules の `$(am__v_CC_$(V))`） | 全 Makefile | 全 Makefile | 0 | — | 必須（configure が「nested variables」を確かめる） |
| 自動変数 `$@ $< $? $*`、`$(@D)` | あり | あり | `$@ $?` | — | 必須 |
| suffix rule（`.c.o`・`.c.lo`・`.test.log`・`.log.trs`・`.y.c`・`.sh.log` ほか） | あり | あり | 0 | — | 必須 |
| `%:: %,v` などの 4 行（GNU の内蔵 RCS・SCCS rule を取り消す、recipe の無い終端の match-anything rule） | 35 | 5 | 0 | — | 読んで無視できれば足りる |
| `.PHONY`・`.SUFFIXES`・`.PRECIOUS`・`.MAKE`・`.NOEXPORT`・`.INTERMEDIATE` | あり | あり | 0 | `.NOTPARALLEL`・`.DEFAULT_GOAL` | 受け付ける（`.MAKE` は -n でも実行、他は意味どおりか無視） |
| recipe の prefix `@`・`-`（`+` は automake の `am__make_dryrun` の行） | あり | あり | あり | — | 必須 |
| `$(MAKE)` の再帰と `MAKEFLAGS` の解析（`am__make_running_with_option` が `MAKEFLAGS` から `-n`・`-k` を読む） | 75 | 26 | 2 | — | 必須 |
| double-colon rule | 上の `%::` だけ | 同じ | 0 | — | POSIX として実装 |

configure が make を確かめる項目: 「make sets $(MAKE)」「make supports nested variables」「make supports the include directive (GNU style)」。
依存の追跡の `.Plo` は config.status が作り、無ければ `am--depfiles` の rule が作る（`$(@D)` と `$@-t`）。

結論: **automake の出力と手書きの zlib は POSIX make（2024 版の `include`・`::=`・`+=`・`?=`・`!=` を含む）と、入れ子の変数参照、
いくつかの GNU の特別な target の受け付けで動く。** GNU の関数・条件・`define` を使うのは coreutils の `GNUmakefile` と、それが読む
gnulib の `maint.mk`（maintainer 用、`$(call)`・`$(eval)` 相当の `define`・`$(origin)`・`$(shell)`・order-only ほか）だけ。

## 設計

### 決定

1. **読む Makefile は `makefile`、次に `Makefile`**（POSIX と BSD の make と同じ）。`GNUmakefile` は読まない。名前のとおり GNU make 専用で、
   coreutils ではそれが gnulib の maint.mk を読み、GNU make の全体（`define` と関数の組み合わせ）を求める。automake の `Makefile` だけで build・check・install はできる。
2. **`-j` と jobserver は無い**（ユーザー指示）。`-j`・`-jN`・`-j N`・`--jobs` は受け付けて無視し、`MAKEFLAGS` にも載せない（package の script が渡すことがあるため）。
3. **GNU make だと名乗らない**: `MAKE_VERSION`・`MAKE_HOST` を定義しない。automake の `am__is_gnu_make` は偽になり、`MFLAGS` ではなく `MAKEFLAGS` を解析する。
   `CURDIR`・`MAKELEVEL`・`MAKECMDGOALS`・`MAKEFILE_LIST`・`.DEFAULT_GOAL` は定義する（GNU の Makefile と package の script が読む）。
4. **`MAKEFLAGS` の形は GNU と同じ**: 1 文字の option を先頭に dash 無しでまとめ（`ks`）、command line の macro を ` -- V=1` の後に置く。`MFLAGS` は `-ks`。
   読むときは dash の有無のどちらも受け付ける（POSIX）。
5. **message の文言は GNU make に合わせる**（`make: *** No rule to make target 'x', needed by 'y'.  Stop.`、`make: 'x' is up to date.`、
   `make: Nothing to be done for 'x'.`、sub-make の `make[1]: Entering directory '...'`）。script と人が見慣れた形で、差分試験は stdout の make 自身の行を比べない。
6. **内蔵の rule と macro は GNU の形の一部**: `.c.o` は `$(COMPILE.c) $(OUTPUT_OPTION) $<`（`COMPILE.c = $(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c`、
   `OUTPUT_OPTION = -o $@`）、`.c:`・`.c.a`・`.y.c`・`.l.c`・`.sh:` と POSIX の既定 macro（`CC = cc`、`AR = ar`、`ARFLAGS = rv`、`YACC`、`LEX`、`MAKE`）。
   RCS・SCCS の rule は持たない（だから `%:: %,v` は取り消すものが無く、読んで捨てる）。
7. **recipe の行ごとに `$(SHELL) -c 行`**（`SHELL` の既定は `/bin/sh`、環境の `SHELL` は使わない。GNU と POSIX の通り）。`.POSIX` があれば `-e` を付ける。`.ONESHELL` は範囲外。
8. **Makefile の作り直し（GNU）**: 読んだ Makefile（include したものを含む）を先に goal として更新し、どれかを作り直したら同じ引数で自分を exec し直す
   （`MAKE_RESTARTS`）。無い include file に rule があれば、そのとき作る。automake の `Makefile: Makefile.in config.status` と `.Plo` のため。
9. **時刻は nanosecond**（`st_mtim`、zedBSD の `struct stat` にある）。無い file は古いものとして作る。recipe も file も無い target（`stamp:`）は
   作られたものとする（GNU）。`.PHONY` は常に作る。
10. **範囲外**: `.ONESHELL`、`.SECONDEXPANSION`、grouped target（`&:`）、`--output-sync`、`load`、guile、関数 `file`・`let`・`intcmp`、
    archive の member（`lib.a(member.o)`、POSIX にはあるが、調べた package は使わない）。範囲外は Future Work に置く。

### file の分け方（`userland/base/make/`、Zlib、`plan/coding-style.md` の全文）

| file | 役割 |
| --- | --- |
| `make.h` | 共有の型（`struct target`、`struct variable`、`struct rule`、`struct recipe`、option）と関数の宣言 |
| `main.c` | 引数と `MAKEFLAGS`、環境の取り込み、Makefile の選び方、内蔵の macro と rule、goal の順、Makefile の作り直しと exec、終了 status |
| `read.c` | Makefile を論理行に（継続行、comment、tab の recipe）、指令（`include`・`-include`・`sinclude`、条件、`define`、`export`・`unexport`・`override`、`vpath`）、代入の種類、rule の行、target ごとの変数 |
| `variable.c` | 変数の表（hash）、flavor（再帰・単純）、origin（default・environment・file・command line・override・automatic）、target ごとの scope の連鎖、export |
| `expand.c` | `$(...)`・`${...}`・`$x` の展開、入れ子の参照、置換参照 `$(V:a=b)`、関数の呼び出しの振り分け |
| `function.c` | 関数（p003）: `subst`・`patsubst`・`strip`・`findstring`・`filter`・`filter-out`・`sort`・`word`・`wordlist`・`words`・`firstword`・`lastword`・`dir`・`notdir`・`suffix`・`basename`・`addsuffix`・`addprefix`・`join`・`wildcard`・`realpath`・`abspath`・`if`・`or`・`and`・`foreach`・`call`・`eval`・`value`・`origin`・`flavor`・`shell`・`info`・`warning`・`error` |
| `rule.c` | target の表、prerequisite（通常と order-only）、double-colon、特別な target、suffix rule を pattern rule に変換（GNU と同じ）、暗黙の rule の探索（pattern の一致、最短の stem、中間の file を 1 段まで連鎖）、VPATH・vpath の探索 |
| `update.c` | 更新の算法: 深さ優先で左から、循環の検出、時刻の比較、`-n`・`-t`・`-q`・`-k`・`-i`・`-s`、`.DEFAULT`、自動変数の設定 |
| `job.c` | recipe の 1 行の実行: prefix `@`・`-`・`+`、echo、`fork`・`exec`（`$(SHELL) -c`）、`wait`、signal（SIGINT・SIGTERM で作りかけの target を消す、`.PRECIOUS` は残す）、`$(MAKE)` を含む行は `-n` でも実行 |

### 試験

- 差分試験 `plan/ws046/tests/make-diff.py`: case（`cases/*.sh`）を dash で走らせ、GNU make と我々の make の stdout と status を比べる（make 自身の
  message の行と、`$(MAKE)` の path は除く）。今 91 件: POSIX 45（`posix.sh`）、automake の idiom 15（`automake.sh`）、GNU の機能 31（`gnu.sh`）。
  GNU make だけで全て status 0（下の検証）。
- 実 package（p004）: host で我々の make を build し、expat・zlib・coreutils を `configure && make && make check && make install DESTDIR=` して、
  GNU make のときと install される file の一覧と中身、`make check` の結果を比べる。その後 guest で同じことをする。

### Phase の範囲（WS046 の表を直す）

- **p002 POSIX make の核**: `posix.sh` の 45 件（読む・変数と展開（入れ子、置換参照、`::=`・`+=`・`?=`・`!=`）・explicit rule・suffix rule・
  double-colon・特別な target・`include`・更新の算法・recipe の実行・option と `MAKEFLAGS`・再帰・内蔵の rule）。host で build して差分試験。
- **p003 automake の idiom と GNU の機能**: `automake.sh` の 15 件と `gnu.sh` の 31 件（`:=`、条件、`define`、pattern rule、関数、target ごとの変数、
  `export`、`override`、VPATH、order-only、Makefile の作り直し、無い include の作成、long option）。
- p004・p005 は今の表のまま。

人間の判断が要る点は無い（決定 1〜3 は WS046 の範囲の「autotools の出力を実行できる程度」「並列は不要」からの技術的な判断）。

## 検証

| 検証 | 結果 |
| --- | --- |
| 機能の数え方 | `make-features.py` を expat（2 通り）・zlib・coreutils（Makefile と maint.mk を分けて）に（`evidence/features.txt`） |
| GNU make での実 build | expat: `make`・`make check`・`make install DESTDIR=` が status 0 |
| 差分試験の土台 | `make-diff.py`（GNU make だけ、`--show` で出力を読んで確かめた）: 91/91 |

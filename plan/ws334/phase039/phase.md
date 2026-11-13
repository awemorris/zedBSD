<!-- awesome-plan project=zedbsd record=ws334-p039 -->

# ws334-p039 素の tree の `make browser-win32` が build できない件（gcc の未初期化の error、top の入口）

- 親: [WS334](../ws.md)。2026-11-13 Q1 が q2049-i01 で P3 に割り当て（ユーザーから、P3 で直す、という趣旨の指示）。
- Status: in-progress（調べ・試験済み、cleared は Q1。2026-11-13 P3 q2049-i01、基点 main 405a70d176）
- 依存: なし（p020・p036 の build の形の上）。

## 目的

ユーザーが `mk/foreign.mk` に足した入口 `make browser-win32`（`$(MAKE) -f apps/browser-win32/Makefile`）が、uninitialized variable などの error で build できない。原因を直す。
Q1 の追加の受け入れ（2026-11-13、CI で使うというユーザーの趣旨）: 素の clone で、引数も config.mk も無い `make browser-win32` だけで、tty に聞かず、llvm-mingw の取得も含めて止まらずに build できる事。
source で直せる所は直し、top の `Makefile`・`mk/`・`apps/browser-win32/Makefile` の変更は差分の案にする（当てない）。

## 結果（2026-11-13、P3 q2049-i01）

### 原因

素の tree（config.mk 無し）の `make browser-win32 < /dev/null` は 2 段で止まる。

1. **top の `Makefile`**: PLATFORM が空なので `/dev/tty` に聞き（tty が無いと空のまま）、`include $(CONFIGMK)` が config.mk の無い tree で `No rule to make target 'config.mk'` で止まる（rc 2）。compile まで行かない。
   config.mk（PLATFORM 入り）のある tree では、ここを越えて 2. に行く。
2. **`apps/browser-win32/Makefile` の compiler の選び**: `WIN32_CC`・`LLVM_MINGW` が無く、PATH に `<triple>-clang`（llvm-mingw）も無いと、x86_64 は Debian の `x86_64-w64-mingw32ucrt-gcc`（GCC 14）に落ちる。
   gcc の `-O2 -Wall -Wextra -Werror` で `-Wmaybe-uninitialized` が 105 行（93 箇所、16 file）出て止まる。これが「uninitialized variable の error」。aarch64 は落ちる先が無く `no Windows compiler` の error。
   llvm-mingw（clang 23.1.4）ではこの warning は出ない: clang の未初期化の診断（`-Wsometimes-uninitialized`）は関数の中の制御の流れだけを見て、address を渡した変数（`&document`）を未初期化と扱わない。gcc の `-Wmaybe-uninitialized` は inline の後の最適化の段で働き、別の翻訳単位の関数（`bind_throw_illegal`・`vm_throw_type_error`）の返す値を知らないので、「失敗の道で 0 が返り得る」とみなす。

### gcc の `-Wmaybe-uninitialized` 93 箇所の読み（全部誤検出、source は直さない）

各件で、変数を書く関数（取り出しの関数）の全部の `return` と、使う側の道を読んだ。本当に未初期化で使われる道は無かった。Q1 の指示により、gcc の誤検出のための書き換え（黙らせるだけの初期化）はしない。

| 箇所（gcc の行） | 変数を書く関数 | 誤検出の理由 |
| --- | --- | --- |
| `bind/document.c` 21 箇所（452・598・650・716・772・929・963・994・1025・1058・1246・1277・1312・1504・1681・1761・1805・1853・1948・2173・2226、929 は `document_find_title` の inline） | `document_this` | 成功の道だけ `return 0` の前に `*document` を書く。失敗の道は `bind_throw_illegal` の値を返し、それは `vm_throw_type_error` → `vm_throw_error` → `vm_throw` か `operation_throw_text`（どれも `VM_THROWN` = -1）で、0 にならない。呼ぶ側は `status != 0` で戻る |
| `bind/element.c` 16 箇所（373・477・531・561・601・823・998・1024・1277・1305・1328・1528・1683・1889・1959・1996、1328 は `element_attribute_name`、1683 は `image_state` の inline） | `element_this` | 同じ（失敗は `bind_throw_illegal` = -1） |
| `bind/markup.c` 8 箇所（75 は `bind_template_contents` の inline・124・160・213・245・308・367・431） | `markup_element_this` | 同じ |
| `bind/geometry.c` 6 箇所（227・276・658・1005・1263 は `geometry_is_root` の inline・1796） | `geometry_element_this` | 同じ |
| `bind/text.c` 7 箇所（408・451・481・512 の `text`、659・684・709 の `doctype`） | `text_this`・`doctype_this` | 同じ |
| `bind/query.c` 3 箇所（181・237・365） | `query_element_this` | 同じ |
| `bind/media.c` 3 箇所（145・317・354） | `media_this` | 同じ |
| `bind/exception.c` 3 箇所（362・387・412） | `exception_of` | 4 つの失敗の道とも `bind_throw_illegal` |
| `bind/range.c` 3 箇所（1880・1882・1892） | `range_this` | 同じ（3 つの失敗の道） |
| `bind/encoding.c` 2 箇所（491・516） | `encoding_decoder_of` | 同じ（4 つの失敗の道） |
| `bind/cssom.c` 623 | `cssom_this` | 同じ（4 つの失敗の道） |
| `bind/select.c` 2 箇所（308・388） | `select_this` | `bind_this_node` の失敗は `bind_throw_illegal`。`select_html` が偽の時は `bind_throw_illegal` の後 `if (status != 0) return status; return 0;` で、`*element` を書かずに 0 を返す形が書いてあるが、`bind_throw_illegal` は常に -1 なので到達しない（書き方の冗長、規約の見直し p035 の候補） |
| `js/builtin_typed.c` 4 箇所（1300・1302・1304・1362） | `view_this` | 失敗は `vm_throw_type_error`（-1） |
| `js/builtin_date.c` 5 箇所（603・719・809・1341、1678 は `date_local` の inline）と `vm/vm.h:930`（`date_get_time` の inline） | `date_this_time` | 失敗は `vm_throw_type_error`（-1） |
| `js/builtin_number.c:221` と `vm/vm.h:930`（`number_value_of` の inline） | `boolean_this`・`number_this` | 失敗は `vm_throw_type_error`（-1） |
| `html/parser.c` 1765（`bookmark`）・1811（`list_index`）・1678（`node_index`） | `tb_formatting_index`・`tb_open_index`（`tb_adoption_agency` の中、戻り値を見ない呼び） | 不変条件: formatting element は `tb_formatting_after_marker` が list から返した物で、内の loop は formatting element に着くと break し、list から外すのは他の node だけなので、1748 と 1809 の時に list にある。furthest block は `p->open` を走って見つけた物なので `tb_open_index` が見つける |
| `paint/gradient.c:126`（`at`、`gradient_positions` の inline） | `gradient_positions` の最初の loop | `at[0]` を読むのは `count >= 1` の時だけ書かれる。`css_gradient_parse` が `count < 2` を EINVAL で拒み、`cascade_gradient` は count を写すだけなので、描く gradient は常に 2 つ以上の stop |
| `libmedia/decoder.c` 183・737（`index`） | `decoder_try` | `*index` を書かずに返すのは `ENOMEM`・open の 0 以外の status・`first`（`MEDIA_PROBLEM_FORMAT` 1003 か DEVICE・PROFILE・BUSY）で、どれも 0 でない |

### gcc に落ちる道は元から成り立たない

`-Wno-error=maybe-uninitialized` を足して gcc で最後まで流した（`WIN32_CC="x86_64-w64-mingw32ucrt-gcc -Wno-error=maybe-uninitialized" WIN32_TOOL_PREFIX=x86_64-w64-mingw32-`）。

- link: `x86_64-w64-mingw32ucrt-ld: unrecognized option '--delayload=vulkan-1.dll'`（vulkan-1.dll の遅延の import は lld の機能で、GNU ld に無い）。
- dist: license の写しが `/usr/LICENSE.TXT`・`/usr/x86_64-w64-mingw32/share/mingw32/COPYING.*.txt` を求めて `No rule to make target`（llvm-mingw の tree の配置が前提）。

よって gcc の道は p036（1 つの .exe・遅延の import）以後 build できない。直すのは source でなく compiler の選び（下の案 C）。

### Makefile の差分の案（当てていない、ユーザーの確認事項）

素の clone（scratch の `git clone`、config.mk 無し）に 3 つを当てて `make -j4 browser-win32 < /dev/null` を流し、確かめた（下の「確かめ」）。

- **案 A（top の `Makefile`）**: command line の goal が全部 foreign の target（`keiland-linux`・`keiland-linux-install`・`keiland-freebsd`・`keiland-freebsd-install`・`browser-win32`）の時は、`mk/config.mk`・config.mk・PLATFORM の問い・機種の mk を読まず、`mk/foreign.mk` だけを読む。他の goal（goal 無しの `all` を含む）は今と同じ。
  理由: foreign の target は zedBSD の機種に依らず、tty の無い CI で `/dev/tty` の問いと config.mk の include が止める。makefile-style A.6 に合わせ、`ifeq` の block ごとに comment。
- **案 B（`mk/foreign.mk`）**: `browser-win32` は `LLVM_MINGW` が無ければ llvm-mingw の release 20261020（今 cross-build.sh などが既定にしている版）の host 用（`uname -m` が x86_64 か aarch64 の Linux 用、ubuntu-22.04 の build）を GitHub の release から `build/llvm-mingw/` に 1 度だけ取り、SHA-256 を照合して展開し、`LLVM_MINGW=<それ>` で x86_64 と aarch64 の 2 つを作る（`WIN32_ARCH=` か `BROWSER_WIN32_ARCHES=` で 1 つ）。
  SHA-256 は 2026-11-13 に release から取った tarball の値（x86_64 は host の `~/opt` の tarball と一致）。LLVM の llvm-toolchain-cached と同じく「決めた版を取って照合する」形。
  注意: `BROWSER_WIN32_BUILD=` を渡す時は `WIN32_ARCH=` も渡す（2 つの arch が同じ出力の directory に書くため）。
- **案 C（`apps/browser-win32/Makefile`）**: gcc（`x86_64-w64-mingw32ucrt-gcc`）に落ちる 4 番目の道を外し、`WIN32_TOOL_PREFIX` の gcc の形の置換を外す。error の文に `make browser-win32` が llvm-mingw を取る事を書く。冒頭の comment の古い入口（`make browser-win64`・`browser-winarm64`）を `make browser-win32` に直す。
  理由: 上の「gcc に落ちる道は元から成り立たない」。残すなら、少なくとも `-Wmaybe-uninitialized` の誤検出（-Werror で止まる）・`--delayload` の無い link・license の path の 3 つを別に扱う必要があり、llvm-mingw に揃える方が小さい。推し: 外す。

```diff
diff --git a/Makefile b/Makefile
--- a/Makefile
+++ b/Makefile
@@ -58,6 +58,29 @@ KERNEL_LTO     := full
 # Indicates that the build is done by the toplevel Makefile
 TOPLEVEL_BUILD := 1
 
+# Targets that build for another system (mk/foreign.mk) need neither a
+# platform nor the configuration file: alone on the command line, they
+# skip both.
+FOREIGN_GOALS := keiland-linux keiland-linux-install keiland-freebsd \
+                 keiland-freebsd-install browser-win32
+
+# Whether every goal on the command line is a foreign target (no goal is
+# the default, all, which is not).
+ifneq ($(MAKECMDGOALS),)
+ifeq ($(filter-out $(FOREIGN_GOALS),$(MAKECMDGOALS)),)
+FOREIGN_ONLY := 1
+endif
+endif
+
+# Only the foreign targets' rules for those goals, everything else for the
+# others.
+ifeq ($(FOREIGN_ONLY),1)
+
+# Foreign targets. (targets: keiland-linux, keiland-linux-install, keiland-freebsd, keiland-freebsd-install, browser-win32)
+include mk/foreign.mk
+
+else
+
 # Import the base configuration variables.
 include mk/config.mk
 
@@ -114,5 +137,7 @@ include mk/platform-$(PLATFORM)/Makefile
 # Rebuild tracking: objects follow their flags and headers.
 include mk/tracking.mk
 
-# Foreign targets. (targets: keiland-linux, keiland-linux-install, keiland-freebsd, keiland-freebsd-install)
+# Foreign targets. (targets: keiland-linux, keiland-linux-install, keiland-freebsd, keiland-freebsd-install, browser-win32)
 include mk/foreign.mk
+
+endif
diff --git a/mk/foreign.mk b/mk/foreign.mk
--- a/mk/foreign.mk
+++ b/mk/foreign.mk
@@ -14,5 +14,41 @@ keiland-freebsd:
 keiland-freebsd-install:
 	$(MAKE) -f userland/desktop/keiland-freebsd.mk install
 
-browser-win32:
-	$(MAKE) -f apps/browser-win32/Makefile
+# OmniGrammaton for Windows (apps/browser-win32), for x86_64 and arm64
+# (BROWSER_WIN32_ARCHES, or WIN32_ARCH=x86_64 or aarch64 for one), with
+# llvm-mingw: LLVM_MINGW=<its root>, or else the release below, fetched
+# into build/llvm-mingw once and checked against its SHA-256 (a build of
+# it for the host's machine, x86_64 or aarch64 Linux).
+LLVM_MINGW_RELEASE        := 20261020
+LLVM_MINGW_HOST           := $(shell uname -m)
+LLVM_MINGW_NAME           := llvm-mingw-$(LLVM_MINGW_RELEASE)-ucrt-ubuntu-22.04-$(LLVM_MINGW_HOST)
+LLVM_MINGW_URL            := https://github.com/mstorsjo/llvm-mingw/releases/download/$(LLVM_MINGW_RELEASE)/$(LLVM_MINGW_NAME).tar.xz
+LLVM_MINGW_SHA256_x86_64  := 55064b60b991a1e1e8090b8c2f84e82b22cfe5903d12c159d65064cf5e8ae335
+LLVM_MINGW_SHA256_aarch64 := ca16bb0f4f485876496fa46ad8d353c525eba57d0cf9e9e49ec1738fe4810151
+LLVM_MINGW_FETCHED        := build/llvm-mingw/$(LLVM_MINGW_NAME)
+BROWSER_WIN32_ARCHES      ?= $(or $(WIN32_ARCH),x86_64 aarch64)
+
+# Whether the build fetches llvm-mingw (no LLVM_MINGW given).
+ifeq ($(strip $(LLVM_MINGW)),)
+BROWSER_WIN32_LLVM_MINGW  := $(abspath $(LLVM_MINGW_FETCHED))
+BROWSER_WIN32_TOOLCHAIN   := $(LLVM_MINGW_FETCHED)/.fetched
+else
+BROWSER_WIN32_LLVM_MINGW  := $(LLVM_MINGW)
+BROWSER_WIN32_TOOLCHAIN   :=
+endif
+
+browser-win32: $(BROWSER_WIN32_TOOLCHAIN)
+	for arch in $(BROWSER_WIN32_ARCHES); do \
+		$(MAKE) -f apps/browser-win32/Makefile LLVM_MINGW=$(BROWSER_WIN32_LLVM_MINGW) WIN32_ARCH=$$arch || exit 1; \
+	done
+
+$(LLVM_MINGW_FETCHED)/.fetched:
+	@if [ -z '$(LLVM_MINGW_SHA256_$(LLVM_MINGW_HOST))' ]; then \
+		echo 'llvm-mingw $(LLVM_MINGW_RELEASE) has no Linux build for $(LLVM_MINGW_HOST): pass LLVM_MINGW=<root of llvm-mingw>'; exit 1; fi
+	@mkdir -p build/llvm-mingw
+	curl -fsSL --retry 3 -o build/llvm-mingw/$(LLVM_MINGW_NAME).tar.xz.part '$(LLVM_MINGW_URL)'
+	@echo '$(LLVM_MINGW_SHA256_$(LLVM_MINGW_HOST))  build/llvm-mingw/$(LLVM_MINGW_NAME).tar.xz.part' | sha256sum -c --quiet - || \
+		{ echo '$(LLVM_MINGW_NAME).tar.xz: the SHA-256 is not $(LLVM_MINGW_SHA256_$(LLVM_MINGW_HOST))'; exit 1; }
+	mv -f build/llvm-mingw/$(LLVM_MINGW_NAME).tar.xz.part build/llvm-mingw/$(LLVM_MINGW_NAME).tar.xz
+	tar -xJf build/llvm-mingw/$(LLVM_MINGW_NAME).tar.xz -C build/llvm-mingw
+	touch $@
diff --git a/apps/browser-win32/Makefile b/apps/browser-win32/Makefile
--- a/apps/browser-win32/Makefile
+++ b/apps/browser-win32/Makefile
@@ -2,7 +2,7 @@
 # libkeiland and the browser's user interface into Windows programs.
 # Run from the top of the tree: make -f apps/browser-win32/Makefile
 # WIN32_ARCH=x86_64 (or aarch64) or through the top Makefile: make
-# browser-win64, make browser-winarm64.  Output under
+# browser-win32 (both, or WIN32_ARCH= for one).  Output under
 # $(BROWSER_WIN32_BUILD) (build/browser-win64 or
 # build/browser-winarm64 by default).
 #
@@ -30,17 +30,18 @@
 # libbrowser-script.a, the script core alone with its Windows layer,
 # and script-core.exe over it.
 #
-# The toolchain, first found wins:
+# The toolchain is llvm-mingw (its clang, lld and the UCRT runtime), first
+# found wins:
 #   1. WIN32_CC=<compiler>
 #   2. LLVM_MINGW=<root of llvm-mingw>: $(LLVM_MINGW)/bin/<triple>-clang
+#      (make browser-win32 at the top fetches the release it names)
 #   3. <triple>-clang on PATH
-#   4. x86_64 only: x86_64-w64-mingw32ucrt-gcc on PATH (Debian's UCRT mingw-w64)
 #
-# where <triple> is x86_64-w64-mingw32 or aarch64-w64-mingw32.  The
-# msvcrt mingw-w64 gcc (x86_64-w64-mingw32-gcc) is never used: the
-# build targets the UCRT.  Recipes run under a POSIX sh (also on a
-# Windows host: Git for Windows' or MSYS2's).  Copyright (C) 2026 Awe
-# Morris; SPDX-License-Identifier: Zlib
+# where <triple> is x86_64-w64-mingw32 or aarch64-w64-mingw32.  A mingw-w64
+# gcc is not used: its linker has no delayed imports (--delayload), and the
+# licences of the runtime are read from llvm-mingw's tree.  Recipes run
+# under a POSIX sh (also on a Windows host: Git for Windows' or MSYS2's).
+# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 
 WIN32_ARCH ?= x86_64
 ifeq ($(WIN32_ARCH),x86_64)
@@ -59,16 +60,12 @@ ifneq ($(strip $(LLVM_MINGW)),)
 WIN32_CC := $(LLVM_MINGW)/bin/$(WIN32_TRIPLE)-clang
 else ifneq ($(shell command -v $(WIN32_TRIPLE)-clang 2>/dev/null),)
 WIN32_CC := $(WIN32_TRIPLE)-clang
-else ifeq ($(WIN32_ARCH),x86_64)
-ifneq ($(shell command -v x86_64-w64-mingw32ucrt-gcc 2>/dev/null),)
-WIN32_CC := x86_64-w64-mingw32ucrt-gcc
-endif
 endif
 endif
 ifeq ($(strip $(WIN32_CC)),)
-$(error no Windows compiler for $(WIN32_ARCH): install llvm-mingw and pass LLVM_MINGW=<its root> (for example LLVM_MINGW=$$HOME/opt/llvm-mingw), or WIN32_CC=<compiler>)
+$(error no Windows compiler for $(WIN32_ARCH): run make browser-win32 at the top (it fetches llvm-mingw), or pass LLVM_MINGW=<root of llvm-mingw> or WIN32_CC=<compiler>)
 endif
-WIN32_TOOL_PREFIX ?= $(patsubst %ucrt-gcc,%-,$(patsubst %-clang,%-,$(patsubst %-gcc,%-,$(WIN32_CC))))
+WIN32_TOOL_PREFIX ?= $(patsubst %-clang,%-,$(WIN32_CC))
 WIN32_AR ?= $(WIN32_TOOL_PREFIX)ar
 WIN32_DLLTOOL ?= $(WIN32_TOOL_PREFIX)dlltool
 ifeq ($(WIN32_ARCH),x86_64)
```

CI の host が要る物（案の後）: GNU make・POSIX sh・curl・tar（xz）・sha256sum・host の cc（icon の道具 make-ico）・GitHub（llvm-mingw・Vulkan-Headers・Vulkan-Loader の tarball）と ca-certificates の取得先への通信。

### 確かめ

QEMU・実機は使っていない（Windows の cross build と Wine だけ）。

| コマンド | 結果 |
| --- | --- |
| 基点の tree（config.mk 無し）で `make browser-win32 < /dev/null`（`build/p039/top-bare.log`） | rc 2。`cannot create /dev/tty`、`Makefile:65: config.mk: No such file or directory`、`No rule to make target 'config.mk'` |
| `make -k -j4 -f apps/browser-win32/Makefile BROWSER_WIN32_BUILD=build/p039/gcc-win64 < /dev/null`（compiler の指定なし → gcc 14、`build/p039/gcc-win64.log`） | rc 2。`-Werror=maybe-uninitialized` 105 行（93 箇所、16 file）、他の warning・error は無し |
| 同じに `WIN32_CC="x86_64-w64-mingw32ucrt-gcc -Wno-error=maybe-uninitialized" WIN32_TOOL_PREFIX=x86_64-w64-mingw32-`（`build/p039/gcc-win64-nouninit.log`） | rc 2。link で `unrecognized option '--delayload=vulkan-1.dll'`、dist で `/usr/LICENSE.TXT` などの規則なし |
| `make -j4 -f apps/browser-win32/Makefile LLVM_MINGW=$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64 WIN32_ARCH=x86_64`（と `aarch64`）`BROWSER_WIN32_BUILD=build/p039/llvm-<arch> < /dev/null` | 両方 rc 0、`warning:` 0、toolchain.txt の compat と libtruetype の warning 0、`OmniGrammaton.exe` x86_64 3902464 bytes・aarch64 3506176 bytes |
| 案 A・B・C を当てた素の `git clone`（config.mk 無し、PATH に llvm-mingw 無し）で `make -j4 browser-win32 < /dev/null` | rc 0。llvm-mingw 20261020 を取得・SHA-256 照合・展開し、x86_64・aarch64 の両方を build。log の `warning:` 0・error 0、両 arch の toolchain.txt の compat の warning 0、`OmniGrammaton.exe`（PE32+ x86-64 GUI・ARM64 GUI）と dist（share/licenses 13 file）ができた |
| 同じ clone で 2 回目の `make -j4 browser-win32 < /dev/null` | rc 0、compile・link の作り直し 0 |
| 同じ clone で `make browser-win32 WIN32_ARCH=aarch64` | aarch64 の 1 つだけ走る |
| 同じ clone で `make help < /dev/null` | 今と同じく PLATFORM の問いに行く（foreign 以外の goal は変わらない） |
| `sh plan/ws334/tests/cross-build.sh < /dev/null`（基点の tree、`build/p039/cross-build.out`） | FAIL。script が top の `make browser-win64`・`browser-winarm64` を呼び、top の Makefile が PLATFORM と config.mk で止まる（基点に `browser-win64` の target も無い）。script の入口の直しは P2 の担当（Q1 の指示で直していない） |
| `sh plan/ws334/tests/tls-wine.sh < /dev/null`（基点の tree、`build/p039/tls-wine.out`） | PASS（a〜g、rc 0。make は `LLVM_MINGW` の既定の `~/opt` の llvm-mingw 20261020 で x86_64） |

zedBSD 側の libbrowser の build: source を変えていないので流していない（不要）。

### 未実施・残り

- 案 A・B・C の適用（ユーザーの確認の後、Makefile は Claude が変えない）。
- cross-build.sh の入口を `make browser-win32` に合わせる件は P2。案 B の後は、cross-build.sh が `BROWSER_WIN32_BUILD=` を渡す時に `WIN32_ARCH=` も渡す必要がある。
- `bind/select.c` の `select_this` の到達しない `return 0`（`*element` を書かない）は書き方の冗長。規約の見直し（p035）の候補。
- aarch64 の Linux の host での案 B（aarch64 用の llvm-mingw の取得）は、SHA-256 を取っただけで、その host で流していない。

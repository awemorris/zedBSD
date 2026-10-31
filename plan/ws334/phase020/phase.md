<!-- awesome-plan project=zedbsd record=ws334-p020 -->

# ws334-p020 glue: source の一覧の分け（sources.mk）と Windows の build の本体の第 1 段

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §4.1 の 2、§6.1〜§6.3、§6.9、§9。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p004（設計の確定）。module の Phase と並べて起動してよい（module の file に触れない）。
- 担当の想定: Opus（phase-runner）を 1 回。統合の glue なので標準廉価実装手順の module ではないが、手順と受け入れはここに全部書いた。設計の判断を残していない。判断が要る所が出たら止めて Q1 に返す。

## 読む物

1. AGENTS.md の §4・§6・§7・§9・§10、plan/agents/protocol.md、plan/guardrail.md の「範囲」「配置」、plan/coding-style.md（C を変える 2 つの file の分だけ）。
2. design.md の §2.9・§6.1〜§6.3・§6.9・§9.2。
3. 今の Makefile: `userland/desktop/libbrowser/Makefile`、`libmedia/Makefile`、`libtruetype/Makefile`・`Makefile.linux`・`Makefile.freebsd`、`libpng-compat`・`libjpeg-compat`・`libgif-compat` の 3 つの Makefile、`userland/base/libz-compat` の 3 つ、`libkeiland/Makefile`・`Makefile.linux`、`browser/Makefile`、`userland/desktop/package.mk` の `ZEDBSD_USERLAND_PACKAGE`、`userland/desktop/keiland-linux.mk`・`keiland-freebsd.mk`。
4. p002 の骨格: `apps/browser-win32/Makefile`、一番上の `Makefile` の `browser-win64` の規則、`plan/ws334/tests/check-skeleton.sh`・`modules.sh`。

## 作る物と手順

1. **sources.mk**: 次の 9 つの directory に `sources.mk`（変数だけ、copyright の 1 行の comment 付き）を作り、今の Makefile の source の一覧の変数を**そのまま**移す。Makefile は `include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk` で読み、一覧を使う所は変えない。
   | directory | 変数（今の名前を保つ） | sources.mk を読む Makefile |
   | --- | --- | --- |
   | `userland/desktop/libbrowser` | `LIBBROWSER_SOURCES`（`LIBBROWSER_DIR` も sources.mk へ）。加えて `LIBBROWSER_SCRIPT_SOURCES`（base・vm・js の .c、今の一覧の順のまま）と `LIBBROWSER_ENGINE_SOURCES`（残り）に分け、`LIBBROWSER_SOURCES := $(LIBBROWSER_SCRIPT_SOURCES) $(LIBBROWSER_ENGINE_SOURCES)` | `Makefile` |
   | `userland/desktop/libmedia` | `LIBMEDIA_SOURCES`（mediafile の 6 つを含む今の一覧） | `Makefile` |
   | `userland/desktop/libtruetype` | `LIBTRUETYPE_SOURCES` | `Makefile`・`Makefile.linux`（`KEILAND_LINUX_TRUETYPE_SOURCES := $(LIBTRUETYPE_SOURCES)`）・`Makefile.freebsd` も同じ形 |
   | `libpng-compat`・`libjpeg-compat`・`libgif-compat`・`userland/base/libz-compat` | `LIBPNG_COMPAT_SOURCES` などの今の名前 | 各 directory の `Makefile`・`Makefile.linux`・`Makefile.freebsd` |
   | `userland/desktop/libkeiland` | `LIBKEILAND_SOURCES`（今の `+=` の 2 つの塊を 1 つに） | `Makefile`・`Makefile.linux`（`KEILAND_LINUX_KEILAND_SOURCES := $(LIBKEILAND_SOURCES)`。今の 2 つの一覧は同じ物であることを p002 で確かめた。違えば止めて Q1 に） |
   | `userland/desktop/browser` | `KEILAND_BROWSER_SOURCES`（`KEILAND_BROWSER_DIR` も） | `Makefile` |
   - `Makefile.freebsd` が BSD make の時（`.include`）は、BSD make が GNU make の `:=` と `$(dir ...)` を読めるかを見て、読めなければその file は変えず、結果の節に記録する（FreeBSD の build は WS334 の受け入れの外。変えた時は一覧の一致を確かめる）。
2. **一覧の一致（R12）**: 変える前と後で、各 Makefile の一覧の変数の展開が同じ文字列であることを確かめる script `plan/ws334/tests/sources-same.sh` を書く（`make -f <Makefile> -pn` の database から変数を抜く形か、`$(info)` を出す一時の makefile を `build/tmp/` に作る形。前の値は `git show <基点>:<path>` の Makefile から同じ方法で取る）。最後の行 `sources-same: PASS`。
3. **WS074 の list-sources.sh**: `plan/ws074/tests/list-sources.sh` の 2 行の sed の対象の file を `libbrowser/sources.mk`・`browser/sources.mk` に替える（WS074 の file。Q1 に伝えてから。P8 との調整は Q1）。出力が前と同じ行であることを `sources-same.sh` に入れる。
4. **libtruetype と gif の 1 か所**: `libtruetype/companion.c` の open（:250 付近）の flag を `O_RDONLY | O_CLOEXEC | O_BINARY` にし、file の先頭の macro の節に `O_CLOEXEC`・`O_BINARY` が無い system の 0 の定義（`#ifndef O_CLOEXEC` / `#define O_CLOEXEC 0`、`O_BINARY` も同じ。system の名前の `#ifdef` ではなく機能の有無の確かめ）を足し、comment で理由（Windows の CRT は text の mode で開く、close-on-exec が無い）を書く。`libgif-compat/decode.c` の file の口の open（:54 付近）に同じく `O_BINARY`。他は変えない（決定 21 の U7）。
5. **apps/browser-win32/Makefile の第 1 段**（p002 の骨格に足す）:
   - `sources.mk` を include して libtruetype・png・jpeg・gif・z の一覧を読み、`$(BROWSER_WIN32_OBJ)/lib<name>.a` を作る（`$(WIN32_TOOL_PREFIX)ar rcs`）。compat と libtruetype は `-Werror` を外した flags（`WIN32_COMPAT_CFLAGS`、`-Wall -Wextra` は残す）で compile し、warning の数を `cross-build.sh` が数える（design §6.3）。
   - **Vulkan の header と遅延の import**: `fetch` の target が Khronos の Vulkan-Headers と Vulkan-Loader の版を固定した tarball（GitHub の tag の archive、版は `vulkan-sdk-1.4.309.0`。host の libvulkan-dev 1.4.309 に合わせる）を `$(BROWSER_WIN32_BUILD)/sources/` に取り、SHA-256 を照合する（値は取得の時に記録し Makefile に書く。tarball は git に入れない。共有の `build/sources/` には書かない、要るなら Q1 に頼む）。`include/vulkan/`・`include/vk_video/` を `$(BROWSER_WIN32_OBJ)/include/` に写す。Vulkan-Loader の `loader/vulkan-1.def` から普通の import library を `$(WIN32_TOOL_PREFIX)dlltool -m <i386:x86-64 か arm64> -d vulkan-1.def -D vulkan-1.dll -l $(BROWSER_WIN32_OBJ)/libvulkan-1.a` で作り、Vulkan を呼ぶ DLL・.exe の link に `-lvulkan-1 -Wl,--delayload=vulkan-1.dll` を付ける（llvm-dlltool に `--output-delaylib` は無いが、lld の MinGW の driver の `--delayload` が遅延の import を作る。2026-11-01 P7 が llvm-mingw 20261020 と Wine 10 で確かめた: `vulkan-1.dll` がある時は `vkEnumerateInstanceVersion` が 1.4 を返し、`WINEDLLOVERRIDES=vulkan-1=d` の時も .exe が起動して `LoadLibraryExW` の確かめで Vulkan を呼ばずに終わる）。
   - **遅延の import の確かめ**: `plan/ws334/tests/delay-load.c`（試験の program、Win32 の API と `vkEnumerateInstanceVersion` を呼ぶだけ）を `libvulkan-1.a` と `--delayload=vulkan-1.dll` で link し、(a) Wine で `vulkan-1.dll` がある時は版を出して 0 で終わる、(b) Wine の `WINEDLLOVERRIDES=vulkan-1=d`（DLL を無効にする）の時に、program が起動して（遅延の import なので起動の時に落ちない）、`vulkan-1.dll` を `LoadLibraryExW` で確かめて無いと分かり、`vk*` を呼ばずに 0 で終わる、を確かめる。結果（働いたか、働かなければ何が起きたか）を結果の節に書く。
   - **winpthreads の link**（design §6.1 の決め）: DLL と .exe は winpthreads を動的に link し（llvm-mingw の既定は `-lpthread` を明示しないと link しない: 各 DLL・.exe の link に `-lpthread`）、llvm-mingw の `$(LLVM_MINGW)/<triple>/bin/libwinpthread-1.dll` を配る directory に写す（DLL ごとに静的に持つと thread ごとの片付けが 2 組になるため）。単体試験（`run-unit.sh`）は 1 つの .exe なので静的（`-Wl,-Bstatic -lpthread -Wl,-Bdynamic`）。
   - `toolchain.txt` に compat の warning の数も書く。
6. **`plan/ws334/tests/cross-build.sh`**: `make browser-win64`・`make browser-winarm64`（`LLVM_MINGW=`、`BROWSER_WIN32_BUILD=build/tmp/...` の新しい directory）を流し、(1) WS334 の file（layers）の warning 0、(2) compat・libtruetype の warning の数を出す、(3) `$(WIN32_TOOL_PREFIX)nm -u` を許す symbol の表（`plan/ws334/tests/allowed-imports.txt`: UCRT・kernel32・ws2_32・bcrypt・crypt32 の関数の名前。この Phase では compat と libtruetype の .a の未定義の symbol が表の中であること）と照合、(4) 遅延の import の確かめ（上の 5）、を 1 本で。最後の行 `cross-build: PASS`。この script は試験の道具として modules.sh の `ws334_tools` に入れない（統合の Phase が育てる）。

## 変えてはいけない物

- module の header と .c（`modules.sh` の `ws334_headers`・`ws334_sources`）、`run-unit.sh`・`check-skeleton.sh`・`modules.sh`・`unit/`。
- 一覧の中身（source の追加・削除・順）。zedBSD・Linux の build の結果を変えない。
- `config/ci/config-amd64-server.mk`、HAL・toolchain・共有の build/（AGENTS.md §7）。rm をしない。push しない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws334/tests/sources-same.sh` → `sources-same: PASS`。
- zedBSD amd64 の build（libbrowser・libmedia・libtruetype・compat・libkeiland・browser の 9 つの target）: `timeout 2400 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libbrowser.so build/amd64/dynamic/libmedia.so build/amd64/dynamic/libtruetype.so build/amd64/dynamic/libpng-compat.so build/amd64/dynamic/libjpeg-compat.so build/amd64/dynamic/libgif-compat.so build/amd64/dynamic/libz-compat.so build/amd64/dynamic/libkeiland.so build/amd64/bin/browser` → exit 0、warning 0（target の名前は `make -pn` で確かめてから。違えば正しい名前で）。
- Linux の Keiland: `timeout 2400 make keiland-linux KEILAND_ASK=n`（入れる確かめで止まらない形。止まるなら `make -f userland/desktop/keiland-linux.mk all`）→ exit 0。
- WS074 の host の build: `timeout 2400 sh plan/ws074/tests/host-build.sh plain` → 前と同じ結果（P8 の物。件数を前後で数える）。
- `timeout 1800 sh plan/ws334/tests/cross-build.sh` → `cross-build: PASS`。
- `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`。
- `git diff --check`。

## 終わりの条件

- 上が全部 PASS（出力を自分で読む）。遅延の import が働かなかった時は、それを結果に書いて Q1 に返す（予備の thunk は p021 の範囲を変えるので Q1 が決める）。
- `git commit -m WIP -- <変えた path>`。SHA・command と最後の行・未実施を Q1 に。この phase.md に「結果」の節、ws.md の行を「実装済み・試験 PASS」に。

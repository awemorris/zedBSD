<!-- awesome-plan project=zedbsd record=ws334-p036 -->

# ws334-p036 build: 1 つの .exe（DLL を作らない static link）と libkeiland の Windows の pthread 無し

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §6.1・§6.2・§6.6・§6.10。ws.md のユーザーの決定（2026-11-01: Windows では DLL を作らず 1 つの .exe、libkeiland の Windows の port は pthread を使わない）。
- 状態: cleared（2026-11-01 09:04 Q1 の判定、main 8970a6e96。実装済み・試験 PASS。2026-11-01 q1808-i02、P7。p021 の後の同じ Queue の追い、Q1 の 2026-11-01 の指示。cleared の判定は Q1）。
- 2026-11-14 の乖離の確かめ（q2095-i01 P7）: この Phase の .exe の形（DLL を作らない static link）は今も同じ。中身のうち、当面の OpenSSL は libssl-compat・libcrypto-compat に（2026-11-02 の決定、`apps/browser-win32/Makefile` の `WIN32_TLS_LIBRARIES`）、.exe に静的に入れた winpthreads は外され（p040）、vulkan-1.dll の遅延の import は普通の import に（2026-11-13 の決定、cross-build の検査 4）替わった。下の記録は当時の形。
- 依存: p016（platform-win32.c）・p020（`apps/browser-win32/Makefile` と cross-build.sh）・p021（libbrowser-core）。
- 担当の想定: Opus（phase-runner）を 1 回。後の Phase（p022・p023・p026・p028・p030・p031）が同じ形に library と .exe を足す。

## 作る物と手順

1. **design.md §6**（済み、この Phase の最初の commit）: §6.1 の winpthreads（.exe に静的）と libkeiland の pthread 無し、§6.2 の成果物（static library と 1 つの `OmniGrammaton.exe`、headless の mode も同じ .exe、`.def`・`exports.map` の展開は Windows で要らない、名前の衝突）、§6.6 の static の OpenSSL と `net/tls-static.c`・p030 の前の「OpenSSL だけ」の印、§6.8 の静的に入る物の許諾の文、§6.10 の libssl-compat への切り替え（WS335 の design §4.2・§4.4）。後の Phase の phase.md に読み替えの注を 1 行ずつ。
2. **`apps/browser-win32/Makefile`**:
   - 3 つの static library を足す: `libbrowser.a`（今は `LIBBROWSER_CORE_OBJECTS` と libbrowser の Windows の OS の層・storage の module。p022・p023 で network と storage を足して全部になる）、`libmedia.a`（今は `media-os-win32.c`。p026 で全部）、`libkeiland.a`（今は `platform-win32.c`・WS327 の `thread-win32.c`・`socket-win32.c`・`win32/*.c`。p028 で共通の file）。今の compat の 5 つと同じ `WIN32_STATIC_LIBRARY` の規則。
   - program の link の形 `WIN32_PROGRAM`（object と static library を `-static` で 1 つの .exe に。winpthreads は静的、Vulkan は遅延の import、system の DLL の import library）。`OmniGrammaton.exe` は p028 がこの形で足す。今の使い手は試験の program: `make -f apps/browser-win32/Makefile test-program TEST_PROGRAM=<名前> TEST_PROGRAM_SOURCES="<file>..."` が `$(BROWSER_WIN32_BUILD)/tests/<名前>.exe` を作る（design §6.2 の `tests/`）。
   - `runtime` の target と `libwinpthread-1.dll` の写しを外す。
3. **libkeiland の Windows の pthread 無し**: `platform/platform.h` は `_WIN32` の時に `<pthread.h>` を include せず、`struct keiui_lock` は SRWLOCK の入る room（`void *` 1 つ）、`keiui_thread_id` は thread の番号と（join する thread の）handle の組。`platform-win32.c` は SRWLOCK（`InitializeSRWLock`・`AcquireSRWLockExclusive`・`ReleaseSRWLockExclusive`）、`GetCurrentThreadId`、`CreateThread`（始めの関数の組を heap に、detached は handle をすぐ閉じる、joined は `WaitForSingleObject` の後に閉じる）。`keiui_*` の約束（header の comment）は変えない。POSIX の形（`pthread_mutex_t`・`pthread_t`）は今のまま。
4. **cross-build.sh**: (5) libkeiland.a の object が pthread の名前を参照しない（`llvm-nm -u` に `pthread_` が無い）、(6) `test-program` で `plan/ws334/tests/script-tool-main.c` を link した .exe の import（`llvm-readobj --coff-imports`）が Windows の system の DLL・UCRT・遅延の `vulkan-1.dll` だけ（`libwinpthread-1.dll` などの同梱の DLL が無い）、x86_64 は Wine で `script-tool random` と Date の 1 本が動く。

## 変えてはいけない物

- libbrowser・libmedia の source（winpthreads のまま。書き換えは Future Work F-WS334-win32-threads）。module の header（`platform.h` を除く。platform.h は約束を変えず型の中身だけを OS ごとに）、試験の module の .c。
- zedBSD・Linux・FreeBSD の libkeiland の build（`sources.mk` は変えない。Windows の Makefile が posix の file を除き win32 の file を足す）。HAL・toolchain・共有の build/。rm をしない。push しない。

## 確かめの command（worktree の root で）

- `LC_ALL=C.UTF-8 timeout 3600 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`（test-platform-win32 が新しい platform-win32.c で PASS）。
- `timeout 3600 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh` → `cross-build: PASS`（5・6 を含む）。
- zedBSD amd64 の `build/amd64/dynamic/libkeiland.so`（platform.h の POSIX の形が前と同じ）→ warning 0。Linux の Keiland（`plan/tools/keiland-linux/`）の libkeiland の build → warning 0。
- `git diff --check`。

## 終わりの条件

- 上が全部 PASS。commit（`git commit -m WIP -- <path>`、design と Makefile と libkeiland を分けて）。SHA・command と最後の行・未実施を Q1 に。ws.md の行を「実装済み・試験 PASS」に。

## 結果（2026-11-01、P7、q1808-i02、基点 main 2c7e666e2）

commit（`git commit -m WIP -- <path>`）:

| SHA | 中身 |
| --- | --- |
| d808c0a90 | design.md §6（§6.1・§6.2・§6.6・§6.8・§6.9、新しい §6.10）、後の Phase の読み替えの注、この phase.md、ws.md の行（と p021 の cleared の記録） |
| b96208510 | ユーザーの答え（console 用の `omnigrammaton-cli.exe` は要らない、配るのは `OmniGrammaton.exe` だけ）: ws.md の決定、design §6.2・O29・main.c の表、p028・p032・p034 の注 |
| 2ceb0d091 | `libkeiland/platform/platform.h`（Windows の形: `struct keiui_lock` の room、`struct keiui_thread` の number と handle、`<pthread.h>` を include しない）、`platform-win32.c`（SRWLOCK・GetCurrentThreadId・CreateThread）、`plan/ws334/tests/unit/test-platform-win32.c`（thread の identity を number で比べる） |
| 7bc42df7f | `apps/browser-win32/Makefile`（`libbrowser.a`・`libmedia.a`・`libkeiland.a`、`WIN32_PROGRAM` と `test-program`、`runtime` と `libwinpthread-1.dll` を外した、archive は q で作る、header の写しを compat に限る）、`plan/ws334/tests/cross-build.sh`（5・6） |

確かめ:

- `LC_ALL=C.UTF-8 timeout 3700 flock /tmp/zedbsd-wine.lock timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`（34 本、FAIL 0。test-platform-win32 は 315 checks。log `build/tmp/p036/run-unit.log`）。
- 新しい platform-win32.c の変異 4 つ（take の Acquire を外す・give の Release を外す・`keiui_thread_is` の比較を逆に・join の Wait を外す）は全部 test-platform-win32 が見つけた（Release を外した物は試験が止まり、300 s の時間切れ）。元の file に戻した事を `cmp` で確かめた。
- `timeout 3700 flock /tmp/zedbsd-wine.lock timeout 3600 sh plan/ws334/tests/cross-build.sh` → `cross-build: PASS`。win64・winarm64 とも: compat の warning 0、`libkeiland.a refers to no pthread_ name`、`one .exe, imports only the system's DLLs (13)`（KERNEL32・bcrypt・CRYPT32 と UCRT の api-ms-win-crt-* 10 個。`libwinpthread-1.dll` は無い）。win64 は Wine で `script-tool.exe random` が 16 byte、`Date.now()` の script が `number 13`。log `build/tmp/p036/cross-build.log`。
- 手で: `build/tmp/p036/w64b/tests/script-tool.exe`（`make -f apps/browser-win32/Makefile ... test-program TEST_PROGRAM=script-tool TEST_PROGRAM_SOURCES=plan/ws334/tests/script-tool-main.c`）を Wine の `TZ=America/New_York` で p021 の `date.js` → `GMT-0500 (Eastern Standard Time)`・`GMT-0400 (Eastern Daylight Time)`・300。`make -B` で作り直しても同じ header（下の 3）。
- zedBSD amd64 `make ZEDBSD_CONFIG=config/ci/config-amd64.mk ... -j16 build/amd64/dynamic/libkeiland.so`（sysroot は p021 の写し）→ exit 0、warning 0（platform.h を include する file が compile し直された）。
- Linux の Keiland `timeout 2400 make -j16 -f userland/desktop/keiland-linux.mk all KEILAND_LINUX_BUILD=build/tmp/p036/keiland-linux` → exit 0、warning 0。
- `git diff --check` → 出力なし。

自分で決めた点・見つけた事:

1. 「1 つの .exe」の今の形: browser の UI の program（`OmniGrammaton.exe`）は p028 で、その時に `WIN32_PROGRAM` の 1 行で足す。今この形を使うのは試験の program（`test-program`、design §6.2 の `tests/`）だけ。`make browser-win64` は全部の static library を作る。
2. archive の member は file の名前で入るので、`ar rcs` は libbrowser の同じ名前の file（`page/input.c` と `html/input.c` など 18 組）の片方を落とす。新しい archive を毎回作り `qcs`（足すだけ）にした。libbrowser.a は 199 member（core 190 と Windows の層・storage の 9）。
3. p020 の潜在の不具合: `obj/include/%.h` を `include/libc/%.h` から写す pattern の規則が、`make -B` や tree の header の更新の時に、Khronos の `vulkan/vulkan_core.h` を zedBSD の `include/libc/vulkan/` の古い物で上書きしていた（`make -B` で `vulkan_external.h` が無いと compile が止まって分かった）。規則を `include/compat/` に限った。壊れた出力の dir `build/tmp/p036/w64` は使わない（新しい `w64b` で作り直した）。
4. `keiui_thread_id` は Windows で number と handle の組（struct）。join の handle を持つため。比べる時は number（`keiui_thread_is` と試験）。CreateThread は phase.md の通り（同じ library の `thread-win32.c` は `_beginthreadex`。UCRT は CreateThread の thread の CRT の状態も FLS で片付けるので、どちらでも可）。
5. `WIN32_PROGRAM_LDFLAGS` は `-static -lpthread` と system の import library（今は ws2_32・bcrypt・crypt32・shell32・ole32）。後の Phase が使う system の library を足す。

未実施:

- aarch64 の .exe の実行（arm64 の Wine が無い。link と import の表だけ）。実機の Windows。FreeBSD の Keiland の build（platform.h の POSIX の形は変えていない）。
- `OmniGrammaton.exe` 自体（p028）、libmedia・libkeiland の共通の file の static library への追加（p026・p028）、OpenSSL の static library（p030）。

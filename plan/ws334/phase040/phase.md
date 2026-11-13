<!-- awesome-plan project=zedbsd record=ws334-p040 -->

# ws334-p040: Windows 版から winpthreads を外す（thread は Win32 の API で）

Status: cleared（2026-11-13 Q1 判定、Makefile の差分はユーザーの承認の後 main に当てた）。前: in-progress（実装済み・試験 PASS、q2052-i01、P3）
Parent: [WS334](../ws.md)

## 目的

2026-11-13 ユーザーから、次の趣旨の決定を受けた（要点）: winpthreads は本質的に要らない。thread は包んであるはずで、包んでいなければ包むべき。Windows では pthread でなく Win32 の API を使う。winpthreads を link から外す。

## 今の形（Q1 が 2026-11-13 に見た所、確かめ直す事）

- `apps/browser-win32/Makefile` の `WIN32_THREAD_LDFLAGS := -lpthread` が winpthreads を static link している。
- libbrowser は自前の `base/thread.c`・`thread.h` の層を持ち、pthread を直に呼ばない（Windows の実装を確かめる）。
- libmedia は pthread を直に呼ぶ: `aac-codec.c`・`aac-synth.c`・`engine.c`・`picture.c`・`picture.h`・`vkvideo-runtime.c`。OS の層は `media-os.h`・`media-os-posix.c`・`media-os-win32.c`。
- libkeiland は `platform/thread-posix.c`・`platform-posix.c`・`platform.h`（Windows の実装の有無を確かめる）。
- libcrypto-compat の `cryc-os-posix.c`（Windows は別の file か確かめる）。

## 範囲

1. Windows の build に入る全部の source（libbrowser・libmedia・libkeiland・compat・browser-win32 の UI・試験の program）で pthread を直に使う所を一覧にする。
2. 直に使う所は、その library の OS の層（libmedia なら media-os）の thread・mutex・条件変数・once の口に移し、POSIX は pthread、Windows は Win32（`CreateThread`/`_beginthreadex`・`SRWLOCK`・`CONDITION_VARIABLE`・`InitOnceExecuteOnce` など）で実装する。既にある層（libbrowser の thread.h、libkeiland の platform）を使えるなら新しく作らない。library の間の依存を新しく作らない。
3. `-lpthread` を外す差分は Makefile なので案にして返す（`plan/ws334/phase040/makefile.diff`）。確かめは command line で `WIN32_THREAD_LDFLAGS=` を渡して。
4. 確かめ: zedBSD（x86_64）の該当の package の build warning 0、`make browser-win32`（x86_64・aarch64）warning 0、OmniGrammaton.exe と試験の program の import と静的な symbol に winpthreads（`pthread_`・`libwinpthread`）が無い事、Wine での試験（`plan/ws334/tests/` の既存の道具で script・乱数・media が動く事）、libmedia の host の試験。

## 受け入れ

- Windows の .exe に winpthreads が入らない（`WIN32_THREAD_LDFLAGS=` で link が通る）。
- zedBSD の振る舞いは変わらない（POSIX の実装は pthread のまま）。
- Makefile の差分の案がある（当てるのは Q1、ユーザーの承認の後）。

## 結果（2026-11-13 q2052-i01、P3）

基点 main 1f9955f4a2、途中で main 59af1b997a を merge（bb626dc490）。commit: c77c23c43a（libmedia と test-media-os）、b90cfbad8f（cross-build.sh・allowed-imports.txt）、cbea3344c5（Makefile の案・試験の直し・clock の書き方）、末尾は wine-run.sh の M2b を外したのと phase.md の記録の commit。

### 直に pthread を使っていた所と移した先

Windows の build（`apps/browser-win32/Makefile` の全 source と試験の program）を `WIN32_THREAD_LDFLAGS=` で link し、未定義になった名前と grep で数えた。

| library | 直に使っていた所 | 移した先 |
| --- | --- | --- |
| libmedia | `engine.c`（thread の作りと join、mutex・cond、`pthread_cond_timedwait`、`clock_gettime`、`nanosleep`）、`picture.c`・`picture.h`（pool と GPU の slot の mutex、once）、`aac-codec.c`・`aac-synth.c`（once）、`vkvideo-runtime.c`（once、`clock_gettime`） | `media-os.h` に `media_os_mutex`・`media_os_cond`・`media_os_once`・`media_os_thread` と `media_os_clock_ns`・`media_os_sleep_ms` を足した。POSIX（`media-os-posix.c`）は pthread・`clock_gettime`・`nanosleep` のまま、Windows（`media-os-win32.c`）は SRWLOCK・CONDITION_VARIABLE・InitOnceExecuteOnce・`_beginthreadex`・QueryPerformanceCounter・Sleep。header は libkeiland の `platform.h` と同じく `#ifdef _WIN32` で room（1 pointer）を持ち `<windows.h>` を含まない。engine の thread の入口は呼び手の table と自前の thread で同じ `engine_run` 1 つにした |
| libbrowser | 無し（`base/thread.c` は program の table、`base/once.c` は atomic） | 変えない |
| libkeiland | Windows は `platform-win32.c`・`thread-win32.c`（既に Win32）。`platform.h` は `#ifndef _WIN32` で pthread.h | 変えない |
| libcrypto-compat・libssl-compat | Windows は `cryc-os-win32.c`（既に Win32） | 変えない |
| UI・`apps/browser-win32`・compat の library | 無し | — |
| 試験の program（Makefile の test-program） | `plan/ws334/tests/media-main.c` の `nanosleep`、`protect-main.c` が含む `unit/check.h` の mutex | `media-main.c` は Windows で Sleep、`check.h` の lock は atomic の spin（OS の thread の library に依らない） |

残る pthread（Windows の .exe には入らない）: run-unit.sh の win32 の単体試験（`test-queue.c`・`test-os-net.c`・`test-os-file.c`・`test-tools.c` と `userland/tests/browser-threads/threads-posix.c`）は試験の足場として winpthreads を run-unit.sh が直に link する（`-Wl,-Bstatic -lpthread`）。

### 確かめ

- `make -j4 browser-win32 WIN32_THREAD_LDFLAGS=`（素の mingw-w64 gcc、x86_64、merge の後に再 build）: rc 0、`warning:` 0、compat の `.o.log` も空。OmniGrammaton-full.exe の `nm` に pthread の名前 0、import は system の DLL・msvcrt・vulkan-1.dll（winpthread の DLL 無し）。
- `make -j3 browser-win32 WIN32_ARCH=aarch64 LLVM_MINGW=… WIN32_THREAD_LDFLAGS=`: rc 0、warning 0、pthread の名前 0、import は system の DLL・UCRT・vulkan-1.dll。
- 試験の program 9 本（fetch・tls・cert-store・hit-test・protect・theme・script-tool・browser-cli・media の main）を `WIN32_THREAD_LDFLAGS=` で test-program: 全部 link、warning 0、pthread の名前 0（protect-main は check.h を直す前は pthread_mutex_* が未定義で落ちた）。
- `WIN32_ARCH="x86_64 aarch64" sh plan/ws334/tests/cross-build.sh`（今の Makefile のまま、-lpthread は残るが参照が無いので取り込まれない）: cross-build: PASS。検査 5 を「全 static library が pthread_・clock_gettime・nanosleep を参照しない、OmniGrammaton.exe と script-tool.exe に winpthreads の名前が無い」に広げ、allowed-imports.txt から winpthreads の名前（pthread_*・clock_gettime64・nanosleep64）を外した。zip は 18 file（zip-contents.txt のとおり）、Wine で script-tool の乱数 16 byte と script の時計（number 13）。
- `sh plan/ws334/tests/media-wine.sh`: PASS（host の POSIX の build と Wine の media-tool.exe の画の hash が同じ、engine の thread を含む）。media-tool.exe に winpthreads の名前が無い検査を足した（直す前は nanosleep で winpthreads が 194 の名前ごと入っていた）。
- `sh plan/ws334/tests/wine-run.sh`（OmniGrammaton の zip を Wine＋lavapipe で）: 20 PASS・0 FAIL・6 not run（本物の Windows）、wine-run: PASS。M8（--js 22 file と --dump=layout 3 頁が host と同じ）、M1・M2a・M3・M4・M5・M6・M6p（protect-main 38 checks）・M7・caption・dpi。1 回目は M2b（vulkan-1.dll 無しで CPU の描画）が FAIL: vulkan-1.dll は普通の import になった（ユーザーの決定、遅延の読み込みは要らない）ので program は vulkan-1.dll 無しでは起動しない。今の決定と食い違う古い期待なので M2b を外した（今回の変更とは無関係）。
- `sh plan/ws334/tests/run-unit.sh media-os`: posix 184・win32 179 checks PASS（thread・mutex・cond の時間切れ・once・clock・sleep の検査を足した）。
- `sh plan/ws074/tests/host-build.sh plain`（warning 0）と `sh plan/ws334/tests/media-wake-test.sh plain`: PASS（試験の `media_engine_open` の呼び方が古かったので直した）。
- zedBSD x86_64: `make PLATFORM=x86_64 llvm-toolchain-cached`・`sysroot` の後、`build/x86_64/packages/libmedia/.stamp`・`media-probe`・`videoplayer`・`libbrowser` の package を `-j4` で build、warning 0。

### Makefile の差分の案

[makefile.diff](makefile.diff): `WIN32_THREAD_LDFLAGS := -lpthread` の行、`WIN32_PROGRAM_LDFLAGS` の中のその参照、`flags` の echo の 1 行を消す（`git apply --check` 通過）。当てるのは Q1（ユーザーの承認の後）。

### 未実施・残り

- 本物の Windows での実行（Wine だけ）。aarch64 は link まで（arm64 の Wine が無い）。
- run-unit.sh の win32 の `files`・`os-file`・`queue` は前から build できない（今回の変更と無関係）: `threads-posix.h` が `<browser/browser.h>` を含み、それが `<vulkan/vulkan.h>` を要るが run-unit.sh の win32 に Vulkan の header の path が無い。path を足しても `threads-posix.c` が Windows に無い `PTHREAD_STACK_MIN` で止まる。posix の `queue` は PASS（check.h が pthread.h を含まなくなったので test-queue.c に include を足した）。
- `plan/ws334/design.md`（§3.4・O3・O5・O10 ほか）と Future Work の F-WS334-win32-threads はまだ「winpthreads を使う」形の記述。libmedia の分はこの Phase で済み、libbrowser は前から table。直すかは Q1。
- `make sysroot` の submake が `-j16` を強いる（make の警告 "-j16 forced in submake"）。4 thread の規則と食い違う（Makefile は変えていない）。

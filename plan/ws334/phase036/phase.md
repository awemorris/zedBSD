<!-- awesome-plan project=zedbsd record=ws334-p036 -->

# ws334-p036 build: 1 つの .exe（DLL を作らない static link）と libkeiland の Windows の pthread 無し

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §6.1・§6.2・§6.6・§6.10。ws.md のユーザーの決定（2026-11-01: Windows では DLL を作らず 1 つの .exe、libkeiland の Windows の port は pthread を使わない）。
- 状態: in-progress（2026-11-01 q1808-i02、P7。p021 の後の同じ Queue の追い、Q1 の 2026-11-01 の指示）。
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

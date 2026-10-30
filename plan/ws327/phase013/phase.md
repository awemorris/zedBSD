<!-- awesome-plan project=zedbsd record=ws327-p013 -->

# ws327-p013 thread の層の Win32 の実装（`thread-win32.c`）と mingw の cross compile

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §3・§5.1・§13。担当: 実装の担当（Sonnet 5.5 high）。依存: p007。
- Status: planned
- Disposition: normal
- ユーザーの決定 3（ws.md、原文）: 「P2ですが、Windows用の実装を書いてしまってOKです。インタフェースがWindowsでも通用する形になっているかのチェックです。動作確認は不要です。mingwは使ってよいです。」
  → mingw で compile と link（warning 0）まで。wine でも Windows でも走らせない。

## 1. 読む物

- `AGENTS.md` §4・§7・§10、`plan/coding-style.md` の全文。
- 仕様: `userland/desktop/include/keiland/keiland_thread.h`（各関数の約束。変えない）、design.md §3.2（約束の詳しい形）・§5.1（Win32 への写しの表）。
- 骨格: `userland/desktop/libkeiland/platform/thread-win32.c`（P2 が作った。各関数の comment が Win32 での作り方。record・key の値の record・
  once の受け渡しの型が決めてある）。
- 手本（POSIX の実装、変えない）: `userland/desktop/libkeiland/platform/thread-posix.c`。record の owners の扱いは同じ形にする。
- Win32 の API の宣言: host の `/usr/x86_64-w64-mingw32/include/`（`processthreadsapi.h`・`synchapi.h`・`fibersapi.h`・`winbase.h`・`process.h`）。
- design.md §13（共通の約束。この phase.md の一部として従う）。

## 2. 触ってよい file

| file | 何を |
| --- | --- |
| `userland/desktop/libkeiland/platform/thread-win32.c` | 全ての関数を骨格の comment どおりに書く。static の補助を足してよい。骨格の型（`struct kl_thread`・`struct thread_key_value`・`struct thread_key`・`struct thread_once_call`）は変えてよいが、変えた点を結果に書く |
| `plan/ws327/phase013/phase.md` の「結果」、`plan/ws327/ws.md` の p013 の行 | 記録 |

libkeiland の Makefile には足さない（Windows の build は無い。`build-win32.sh` だけが compile する）。`socket-win32.c` は p014 の物で触らない。

## 3. 作り方の要点（骨格の comment の要約。食い違えば header が正）

| 関数 | Win32 |
| --- | --- |
| `kl_thread_start` | 引数の確かめ（run が NULL・優先度が HIGH より上は EINVAL、`*thread = NULL`）、record を calloc（ENOMEM）、名前を `KL_THREAD_NAME_MAX` で切って写す、owners 2、`_beginthreadex(NULL, (unsigned)stack, thread_main, record, STACK_SIZE_PARAM_IS_A_RESERVATION, &id)`（stack が `UINT_MAX` を超えれば `UINT_MAX` に）。0 なら record を放して EAGAIN。handle と id を record に |
| `thread_main` | `thread_set_name`・`thread_set_priority`、`run(argument)`、`thread_release`、0 を返す |
| `kl_thread_join` | `GetCurrentThreadId() == id` は EDEADLK（record はそのまま）。`WaitForSingleObject(handle, INFINITE)`、`CloseHandle`、free、0 |
| `kl_thread_detach` | `CloseHandle`、`thread_release`、0 |
| `thread_release` | `InterlockedDecrement(&owners) == 0` なら free |
| `kl_thread_is_current` | `GetCurrentThreadId() == id` |
| `kl_thread_sleep` | `Sleep(ms == UINT32_MAX ? UINT32_MAX - 1U : ms)`（`INFINITE` を避ける） |
| `kl_thread_yield` | `(void)SwitchToThread()` |
| `kl_thread_cpu_count` | `GetCurrentProcessorNumberEx(&number)`、`GetActiveProcessorCount(number.Group)`、0 は 1 |
| mutex | `InitializeSRWLock`・`AcquireSRWLockExclusive`・`TryAcquireSRWLockExclusive`（0 は EBUSY）・`ReleaseSRWLockExclusive`、init は 0、destroy は何もしない |
| cond | `InitializeConditionVariable`、`SleepConditionVariableSRW(c, l, INFINITE, 0)`、`wait_for` は `UINT32_MAX` を 1 引き、FALSE で `GetLastError() == ERROR_TIMEOUT` は ETIMEDOUT（他の FALSE も 0: 偽の起きとして扱う）、`WakeConditionVariable`・`WakeAllConditionVariable` |
| `kl_once` | `struct thread_once_call call = { run };`、`InitOnceExecuteOnce(room, thread_once_run, &call, NULL)`、callback は `call->run()` して TRUE |
| key | create: `FlsAlloc(thread_key_destroyed)`、`FLS_OUT_OF_INDEXES` は EAGAIN、room に index と destroy。set: `FlsGetValue` の record、無ければ malloc（ENOMEM）して destroy を写し `FlsSetValue`（失敗は free して ENOMEM）、value を入れて 0。get: record の value か NULL。delete: `FlsFree`。callback: record が NULL でなく value と destroy が NULL でなければ `destroy(value)`、record を free |
| 名前 | `GetModuleHandleW(L"kernel32.dll")` と `GetProcAddress(..., "SetThreadDescription")`、関数の pointer の型に写す（`FARPROC` からの cast は `(void *)` を経る）。UTF-8 → UTF-16 は `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)` を 16+1 の `WCHAR` に。失敗は何もしない |
| 優先度 | `SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL / ABOVE_NORMAL)`、NORMAL は何もしない、失敗は無視 |

## 4. 確かめの command

1. `sh plan/ws327/tests/build-win32.sh` → `compile thread-win32.c: PASS (warnings 0)`・`compile socket-win32.c: PASS`・
   `link win32-link.exe: PASS`・`ws327-win32: PASS`（flags は `-std=gnu17 -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes
   -Wmissing-prototypes`、Windows 10）。
2. `sh plan/ws327/tests/check-base.sh` → PASS。
3. POSIX の build に影響が無い事: `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws327-p013-linux build/ws327-p013-linux/lib/libkeiland.so`
   → 0（`thread-win32.c` は Makefile に無いので compile されない。変わらない事の確かめ）。

## 5. 突然変異の確かめ（§13.6 の Win32 の形）

動かさないので、build の確かめが捕まえる物を見る:

1. 関数を 1 つ消す（例: `kl_cond_broadcast`）→ link が FAIL。
2. 使わない局所の変数を 1 つ足す → compile が FAIL（warning 0 が守られている事）。
3. 戻して `cmp`。

## 6. 注意

- 規約の全文に従う（前方の宣言、各関数の comment、宣言を先頭に、条件の中で関数を呼ばない、成功の return を最後に）。
- `-Wshadow`: 引数の名前 `thread` などが Win32 の宣言を隠さないか。隠すなら名前を変える。
- `_beginthreadex` の stack は `unsigned`、`SetThreadDescription` は無い system がある（Windows 10 1607 より前）: run time に探す。
- `struct kl_mutex` などの room の中に Win32 の物を置く（`thread_lock` などの補助）。`_Static_assert` は残す。
- 写せない所・header の約束を Win32 で守れない所を見つけたら、直さずに結果の「Q1 へ」に書く（ユーザーの決定 3 の目的: interface が
  Windows で通用するかの確かめ）。例: key の destroy の回数（1 回）、`FlsFree` が生きた値の destroy を呼ぶ事（header は未規定と書く）。
- source の comment に開発の管理情報（WS・Phase の番号、plan/ への参照）を書かない。骨格の「Not written yet.」の comment は、書いた
  関数では中身の comment に置き換える。

## 7. 受け入れ（Q1 が見る）

§4 の 1〜3 が PASS、§5 の 1・2 が捕まる、規約の見直し、`git commit -m WIP -- userland/desktop/libkeiland/platform/thread-win32.c
plan/ws327/phase013/phase.md plan/ws327/ws.md`。動作の確かめはしない（決定 3）。

## 結果

（実装の担当が書く）

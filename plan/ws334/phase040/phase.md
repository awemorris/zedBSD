<!-- awesome-plan project=zedbsd record=ws334-p040 -->

# ws334-p040: Windows 版から winpthreads を外す（thread は Win32 の API で）

Status: planned
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

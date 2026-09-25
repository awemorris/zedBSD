<!-- awesome-plan project=zedbsd record=ws061p011 -->

# ws061-p011: WS061 の変更の規約の適合

Phase ID: `ws061-p011`
Parent: [WS061](../ws.md)
Status: planned
Queue: none
Disposition: normal

## 目的

WS061（p005〜p010）で変えた source を [coding-style.md](../../coding-style.md) の全文で見直す（2026-09-26 ユーザー「規約適合は最後でいいです」）。WS061 の完了の条件。

範囲: `src/kern/sched.c`・`filedesc.c`・`poll.c`・`process.c`・`readahead.c`・`vmspace.c`・`buf.c`・`lock.c`（`spin_trylock`）、`src/hal/amd64/`（`task.c`・`percpu.h`・`defs.h`・`descriptor.c`・`trap.S`・`int.c`・`space.c`・`page.c`・`lib.c`・`dispatch.S`）、`src/rtld/rtld.c`、`userland/base/libc/pthread.c`・`semaphore.c`・`posix.c`・`syscall-amd64.S`、`src/libc/crt/crt0-amd64.S`、`src/rtld/entry-amd64.S`、`userland/packages/lang/clang/Makefile`、`plan/ws061/tests/`。

## 受け入れ

style-check の指摘が 0、全文の規約で見直した記録。build（warning 0）、boot test。

<!-- awesome-plan project=zedbsd record=ws064p003 -->

# ws064-p003: WS064 の変更の規約の適合

Phase ID: `ws064-p003`
Parent: [WS064](../ws.md)
Status: planned
Queue: none
Disposition: normal

## 目的

WS064 で変えた source を [coding-style.md](../../coding-style.md) の全文で見直す（2026-09-26 ユーザー「規約適合は最後でいいです」: 性能の目標の後に行う）。

範囲: `userland/base/make/`（`update.c`・`job.c`・`main.c`・`rule.c`・`make.h`）、`src/kern/lock.c`（mutex）、`src/kern/process.c`・`exec.c`・`syscall.c`（vfork）、`src/kern/vmspace.c`・`vm.c`（fork の一括、destroy、`object_link`）、`userland/base/libc/posix.c`・`signal.c`・`syscall-amd64.S`・`src/libc/crt/crt0-amd64.S`（vfork・posix_spawn）、`userland/base/sh/`（`sh_spawn`）、`include/`（`uapi/syscall.h`・`kern/process.h`・`kern/vmspace.h`・`libc/unistd.h`）、`plan/ws046/tests/cases/parallel.sh`。

## 受け入れ

style-check の指摘が 0、全文の規約で見直した記録。build（warning 0）、make の差分試験 100/100、boot test。

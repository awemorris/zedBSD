# WS022 Phase 003: pthread TLS lifetime and runtime acceptance

Last updated: 2026-09-02

Phase ID: `ws022-p003`

Status: planned; not queued

Parent: [WS022](../ws.md)

Depends on: `ws022-p002`

## Objective

Complete the static TLS lifetime for threads and prove compiler-emitted
`_Thread_local` state on supported x86 runtime paths.

## Work and acceptance

1. Replace temporary static-libc process-global fallbacks with real
   `_Thread_local` storage where the API contract requires per-thread state.
2. Make pthread creation clone the executable TLS initialization image, zero
   the remainder, install a distinct thread pointer before user code, and
   release the mapping exactly once on every exit/failure path.
3. Exercise initialized, zero-filled, aligned, address-taken, and independently
   mutated TLS variables across the initial thread and multiple concurrent
   pthreads, including repeated create/join and injected allocation failure.
4. Verify fork/exec behavior against p001's frozen ownership contract and
   retain existing dynamic executable/rtld regressions.
5. Run one final amd64 and i386 PC/AT QEMU campaign. Record exact ELF and
   runtime evidence before completing WS022; no physical checkpoint is needed.

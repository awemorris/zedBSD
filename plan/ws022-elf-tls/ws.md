# WS022: ELF `PT_TLS` and static thread-local storage

Last updated: 2026-09-02

WSID: `ws022`

Status: planned; not queued

Parent: [master plan](../master.md)

## Objective

Implement the ELF `PT_TLS` program-header contract so statically linked zedBSD
executables can use C thread-local storage without replacing it with
process-global state. The initial thread and every subsequently created
pthread must receive an independent, correctly aligned TLS image initialized
from the executable's file image and zero-filled through `p_memsz`.

This work follows WS021. WS021 deliberately keeps static executables free of
`PT_TLS` while migrating the compiler and linker; WS022 removes that temporary
restriction as a separately testable ELF/runtime change.

## Fixed boundaries

- Treat `PT_TLS` as an ELF loader and thread-runtime contract, not as a source
  generator or compiler workaround.
- Validate `p_filesz <= p_memsz`, alignment, file/memory ranges, integer
  overflow, duplicate segments, and implementation bounds before committing a
  new process image.
- Install the initial thread pointer before entering user code. A failed TLS
  allocation or malformed image fails `exec` atomically.
- `pthread_create()` allocates and initializes a distinct TLS block before the
  child can run; exit and failed creation release it exactly once.
- Preserve the existing zedBSD TCB and `thread_self` syscall boundary unless
  p001 proves that a documented ABI revision is required. Any public ABI
  revision must be recorded before p002 implementation.
- Cover both `x86_64-unknown-zedbsd` and `i386-unknown-zedbsd`. Other
  architectures follow after their project toolchains exist.
- General dynamic TLS allocation for arbitrary post-startup `dlopen()` modules
  is outside the first completion boundary unless p001 shows it is inseparable
  from the existing rtld contract. Such a finding becomes a separate Phase.

## Phase registry

| Phase | Status | Required result |
| --- | --- | --- |
| [`ws022-p001`](phase001-contract-and-fixtures/phase.md) | Planned | Freeze the x86 TLS/TCB layout and malformed/valid ELF fixture matrix against compiler-emitted `PT_TLS` objects |
| [`ws022-p002`](phase002-exec-loader/phase.md) | Planned | Kernel exec validates, maps, initializes, and installs the initial executable TLS image atomically |
| [`ws022-p003`](phase003-thread-runtime-acceptance/phase.md) | Planned | libc/pthread allocates independent TLS per thread and static x86 QEMU acceptance passes |

## Completion conditions

- Compiler-emitted static amd64 and i386 executables retain a valid `PT_TLS`
  segment and run without replacing `_Thread_local` objects with globals.
- Initialized and zero-filled TLS variables have the expected values on the
  initial thread and remain independent across concurrently running pthreads.
- Every malformed, duplicate, overflowing, misaligned, truncated, or
  over-bounds `PT_TLS` fixture fails before the old process image is lost.
- Repeated thread create/join and failed-create paths show no double free,
  stale thread pointer, cross-thread alias, or leaked TLS mapping.
- Focused host/ELF tests, `make -j16`, amd64 QEMU, and i386 PC/AT QEMU pass.

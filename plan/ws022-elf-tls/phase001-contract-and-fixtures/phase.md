# WS022 Phase 001: TLS ABI contract and ELF fixtures

Last updated: 2026-09-02

Phase ID: `ws022-p001`

Status: planned; not queued

Parent: [WS022](../ws.md)

## Objective

Audit the current TCB, rtld, exec, pthread, compiler, and linker behavior; then
freeze the first x86 `PT_TLS` contract and its executable fixtures before
changing loader behavior.

## Work and acceptance

1. Record the amd64 and i386 thread-pointer convention, TCB location, static
   TLS offsets, alignment, and ownership across exec, pthread creation, exit,
   fork, and failed creation.
2. Produce compiler-emitted fixtures for initialized TLS, zero-fill TLS,
   alignment greater than the natural word size, multiple symbols, and
   per-thread mutation. Inspect them with the project `llvm-readelf`.
3. Add malformed fixtures for duplicate `PT_TLS`, `p_filesz > p_memsz`, file
   truncation, non-power-of-two/unsupported alignment, address/size overflow,
   and the chosen implementation size limit.
4. Freeze whether startup shared objects participate in the initial static TLS
   block. If supporting them requires a broader rtld ABI change, extract that
   work as a separate Phase before implementation rather than guessing.
5. Complete when the layout and ownership contract is written here, fixtures
   are repeatable, and no public ABI question remains for p002.

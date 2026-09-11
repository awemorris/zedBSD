# WS021 Phase 004: x86 kernel, userland and target Noct migration

Last updated: 2026-09-02

WSID: `ws021`

Phase ID: `p004`

Combined ID: `ws021-p004`

Status: complete (`q064`)

Parent: [WS021](../ws.md)

Depends on: `ws021-p003`

## Objective

Switch every maintained amd64, i386 PC/AT and i386 PC-98 kernel and userland
compile/link path to the project LLVM installation and the matching sysroot,
including `userland/base/noct`.

## Work packages

1. Separate host-tool variables from target-tool variables. For supported x86
   builds, bind target tools to absolute paths below `build/llvm/bin/` and pass
   the fixed `--target` and `--sysroot` values explicitly.
2. Convert GCC/binutils-only warning, code-generation, assembler, linker and
   objcopy syntax to accepted Clang/LLD/LLVM-tool forms without weakening the
   kernel/user ABI or linker-script assertions.
3. Build amd64 and i386 kernels with Clang/LLD. Kernel-private source includes
   remain direct; target public/UAPI use must not reach host headers.
4. Replace repeated loose target libc/startup inputs in user programs with the
   appropriate sysroot artifacts while retaining present static/dynamic and
   linker-script semantics.
5. Configure target Noct's `zedbsd-amd64` build with
   `build/llvm/bin/clang`, `x86_64-unknown-zedbsd`, LLD, and
   `build/amd64/sysroot`. Replace its raw zedBSD libc source attachment with
   the accepted sysroot boundary where applicable. Host Noct remains
   unchanged and host-built.
6. Migrate helper libraries, generated test ELFs and optional selected
   userland packages in the same target graphs; do not leave quiet PATH-based
   exceptions.
7. Audit build logs and recipes for forbidden target GCC/GNU-binutils/MinGW
   command names and host include/library resolution.

## Verification

- Fresh configured builds compile and link all amd64, PC/AT i386 and PC-98
  kernel/userland artifacts using only recorded `build/llvm` target commands.
- Deliberately poisoning target GCC/binutils names in PATH does not change the
  build; deliberately hiding `build/llvm` causes an immediate actionable
  failure.
- ELF and relocation audits preserve every existing architecture, entry,
  segment, stack, TLS, dynamic-loader, and undefined-symbol contract.
- Target Noct is the canonical static amd64 artifact, uses the sysroot, remains
  byte-identical through package/rootfs staging, and retains the upstream BeUI
  implementation. No QEMU claim is made yet.
- `make -j16` compile/image prerequisites needed before loader assembly pass;
  `git diff --check` passes.
- No QEMU runtime is executed in this Phase.

## Completion conditions

All x86 target kernels and userlands—including target Noct—are migrated in one
source-consistent implementation, with no host target tool fallback and no
remaining compile/link incompatibility deferred to runtime acceptance.

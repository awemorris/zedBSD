# WS021 Phase 002: host LLVM build and installation

Last updated: 2026-09-02

WSID: `ws021`

Phase ID: `p002`

Combined ID: `ws021-p002`

Status: complete (`q064`)

Parent: [WS021](../ws.md)

Depends on: `ws021-p001`

## Objective

Extend top-level `make toolchain` so the host compiler builds both host Noct
and a minimal project-owned LLVM 23.1.0 X86 toolchain installed at
`build/llvm/`.

## Fixed build profile

- CMake source: patched `build/llvm-source/llvm`
- Build tree: `build/llvm-build/`
- Install prefix: absolute `build/llvm/`
- Build type: `Release`
- Projects: `clang;lld`
- Targets: `X86`
- Parallelism: the repository's `-j16` policy
- Disable examples, benchmarks and unrelated projects/runtimes. Disable
  optional host dependencies which are not necessary for the compiler tools.
- Use the host C/C++ compiler only for this host LLVM build and host Noct.

## Work packages

1. Give the LLVM configure/build/install state an identity derived from the
   archive, patch level, host tuple, CMake profile, and compiler identity.
2. Make incomplete or mismatched installations rebuild safely without
   accepting a stale stamp or overwriting an unexpected non-generated tree.
3. Install and expose at least `clang`, `clang++`, `ld.lld`, `lld-link`,
   `llvm-ar`, `llvm-ranlib`, `llvm-nm`, `llvm-objcopy`, `llvm-objdump`,
   `llvm-readelf`, and `llvm-strip`.
4. Keep host Noct's compiler boundary explicit: it remains built by `HOSTCC`
   and does not acquire a target triple or amd64 sysroot dependency.
5. Make `make toolchain` config-free and incremental. A missing target
   toolchain in later builds must produce an actionable failure rather than
   falling back to PATH.

## Verification

- Clean and incremental `make -j16 toolchain` pass without `config.mk`.
- Removing one installed generated tool invalidates/reconstructs the install;
  corrupt state is rejected or rebuilt deterministically.
- `clang --version` and LLVM tool versions report 23.1.0 from `build/llvm/`.
- Both target triples compile a freestanding object; `llvm-readelf` reports
  ELF64 x86-64 and ELF32 i386 respectively.
- A PATH-audit fixture shadows or removes host target `gcc`, `ld`, `ar`, `nm`,
  `objcopy`, and MinGW names without affecting these probes.
- No QEMU runtime is executed in this Phase.

## Completion conditions

`build/llvm/` is a verified, repeatable, complete host installation for the
remaining x86 migration and `make toolchain` still produces a working host
Noct interpreter through the independent host-compiler path.

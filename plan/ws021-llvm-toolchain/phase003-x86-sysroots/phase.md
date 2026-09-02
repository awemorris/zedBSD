# WS021 Phase 003: amd64 and i386 sysroots

Last updated: 2026-09-02

WSID: `ws021`

Phase ID: `p003`

Combined ID: `ws021-p003`

Status: planned

Parent: [WS021](../ws.md)

Depends on: `ws021-p002`

## Objective

Build deterministic public target sysroots for amd64 and the common PC/AT /
PC-98 i386 ABI using only `build/llvm/` target tools.

## Sysroot contract

```text
build/amd64/sysroot/
build/i386/sysroot/
  usr/include/                 zedBSD libc and UAPI headers
  usr/lib/                     startup, libc and compiler runtime artifacts
  usr/lib/zedbsd/<platform>/   accepted linker scripts
```

- `crt0.o` is the existing static startup path; `crt1.o` is the existing
  dynamic startup path. No invented `crti.o`/`crtn.o` ABI is required in v1.
- `libc.a` contains the current target libc/runtime implementation once per
  architecture instead of recompiling the same sources into every command.
- Existing supported dynamic-link artifacts remain buildable and are staged
  where required by current dynamic tests/rootfs, without making shared
  linking the default.
- Required compiler-rt builtins are cross-built from the same verified LLVM
  source and installed or explicitly referenced through the sysroot contract.
- PC/AT and PC-98 use the same headers/startup/libc/compiler runtime. Their
  user/kernel/loader linker scripts remain distinct named platform inputs.

## Work packages

1. Define explicit public-header manifests; do not copy kernel-private headers
   into `usr/include` merely to satisfy accidental dependencies.
2. Cross-compile startup objects, libc, support archives and compiler builtins
   for both triples with matching ABI flags.
3. Install linker scripts with stable names for amd64, PC/AT i386 and PC-98
   i386, while preserving their existing memory-layout contracts.
4. Produce a content/identity stamp from all installed inputs, compiler
   identity, target flags, and source hashes; detect stale or extra generated
   state.
5. Add one tiny static compile/link contract per architecture which consumes
   headers, startup, libc and builtins from the sysroot rather than source-tree
   loose objects.

## Verification

- Header manifests contain every current public libc/UAPI header and no known
  kernel-private implementation header.
- Startup and library artifacts have the expected ELF class, machine, symbol
  and relocation properties under `llvm-readelf`/`llvm-nm`.
- Static smoke links resolve with no undefined compiler builtins or host
  library/search path in the link trace.
- PC/AT and PC-98 sysroot content identity is shared while their linker-script
  identities remain distinct.
- Rebuilding is deterministic at the file-manifest level.
- No QEMU runtime is executed in this Phase.

## Completion conditions

Both sysroots are complete enough for every current x86 kernel/userland
consumer, including target Noct, and no public ABI decision remains before the
bulk migration.

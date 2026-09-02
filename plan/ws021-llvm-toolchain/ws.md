# WS021: reproducible x86 LLVM toolchain and sysroots

Last updated: 2026-09-02

WSID: `ws021`

Status: complete (`q064`)

Parent: [master plan](../master.md)

Shared tests: [WS021 test and evidence index](tests/README.md)

## Objective

Make `make toolchain` build the project-owned host tools required for supported
x86 development, then build every i386/amd64 zedBSD target artifact without a
host-installed target compiler, GNU binutils, or MinGW cross toolchain.

The bootstrap boundary is deliberate:

- the host C/C++ toolchain builds the host Noct interpreter and LLVM 23.1.0;
- the installed LLVM toolchain under `build/llvm/` builds zedBSD kernels,
  userland, the target `/usr/bin/noct`, BIOS loaders, and `BOOTX64.EFI`;
- amd64 target compilation uses `x86_64-unknown-zedbsd` and
  `build/amd64/sysroot`;
- i386 target compilation uses `i386-unknown-zedbsd` and the PC/AT/PC-98 shared
  `build/i386/sysroot`;
- sparcv9 and m68030 remain outside this WS and will use a later project-built
  GCC toolchain.

## Fixed decisions

- Upstream source is the official LLVM project `23.1.0` source tarball,
  `llvm-project-23.1.0.src.tar.xz`, from release tag `llvmorg-23.1.0`.
- zedBSD tracks release identity, acquisition logic, signatures/attestation
  metadata where locally usable, and strict downstream patches; it does not
  commit the archive, extracted LLVM source, build tree, or installation.
- The tracked integration root is `toolchain/llvm/`. Recommended ignored
  locations are:

  ```text
  toolchain/llvm/distfiles/       downloaded release inputs
  build/llvm-source/              verified patched extraction
  build/llvm-build/               host build tree
  build/llvm/                     installed host tools
  ```

- The LLVM build enables `clang` and `lld`, restricts code generation to X86,
  uses a Release configuration, and installs the LLVM binutils-compatible
  tools needed by the repository.
- The accepted target triples are `x86_64-unknown-zedbsd` and
  `i386-unknown-zedbsd`. Board and image Variant do not change the triple.
- The optional environment component is omitted. zedBSD is an ELF OS and does
  not use a `gnu` environment.
- PC/AT i386 and PC-98 share one i386 ABI sysroot. Platform linker scripts and
  image/loader rules remain distinct.
- `build/amd64/sysroot` and `build/i386/sysroot` contain public target headers,
  the existing static and dynamic startup objects, zedBSD libc artifacts,
  required compiler-rt builtins, and platform linker scripts. Kernel-private
  headers remain source-tree inputs rather than sysroot public ABI.
- The target build passes `--target` and `--sysroot` explicitly. It does not
  use wrapper scripts which silently fall back to host compilers.
- Missing or stale `build/llvm`/sysroot state fails with an actionable request
  to run `make toolchain`; an ordinary target build does not silently choose
  `/usr/bin/gcc`, GNU `ld`, host `objcopy`, or MinGW.
- Host Noct remains a bootstrap host program built by the host compiler. The
  target Noct package under `userland/base/noct/` is rebuilt by the new clang,
  LLD, and amd64 sysroot like other zedBSD userland.
- BIOS loaders use the installed Clang integrated assembler, LLD, and LLVM
  binary tools in freestanding mode. `BOOTX64.EFI` uses the same installation's
  PE/COFF-capable Clang/LLD path and no host MinGW installation.
- Implementation Phases use source, compile, link, format, and dependency
  checks. They do not repeatedly boot QEMU. Runtime acceptance is deliberately
  consolidated into p006's final big-bang matrix.

## Phase registry

| Phase | Status | Required result |
| --- | --- | --- |
| [`ws021-p001`](phase001-llvm-source-and-zedbsd-target/phase.md) | Complete (`q064`) | Verified LLVM 23.1.0 acquisition and strict zedBSD OS/triple/driver/compiler-rt patch boundary |
| [`ws021-p002`](phase002-host-llvm-build/phase.md) | Complete (`q064`) | `make toolchain` installs the required X86 Clang/LLD/LLVM tools below `build/llvm/` while host Noct remains host-built |
| [`ws021-p007`](phase007-host-llvm-release/phase.md) | Complete (`q064`) | Publish the validated x86_64 Linux host install as a pinned permanent `rev-0` CI asset |
| [`ws021-p003`](phase003-x86-sysroots/phase.md) | Complete (`q064`) | Complete amd64 and shared i386 public sysroots with startup, libc, compiler builtins, and linker inputs |
| [`ws021-p004`](phase004-x86-target-migration/phase.md) | Complete (`q064`) | All x86 kernels and userland, including target Noct, compile/link only through the project LLVM toolchain and sysroots |
| [`ws021-p005`](phase005-bootloader-toolchain-closure/phase.md) | Complete (`q064`) | BIOS and UEFI loaders plus all x86 disk images build without host target GNU/MinGW tools |
| [`ws021-p006`](phase006-big-bang-qemu-acceptance/phase.md) | Complete (`q064`) | One final consolidated amd64/i386/PC-98 QEMU campaign accepts the migrated toolchain and target Noct |

## Queue strategy

The intended execution Queue contains p001, p002, p007, then p003 through p006.
P001--p005 may use bounded host/unit/compile/link/format checks, but QEMU runtime
is not a dependency between them. P006 runs only after every supported x86
image builds and the no-host-target-tool audit passes. If p006 finds a runtime
defect, use the narrowest diagnostic cell while repairing it, then rerun the
complete matrix once for final acceptance.

## WS completion conditions

- `make toolchain` reproducibly builds host Noct with the host toolchain and
  installs the patched LLVM 23.1.0 X86 toolchain under `build/llvm/`.
- Both zedBSD triples are recognized, normalize correctly, select ELF, and
  publish the required zedBSD preprocessor/driver behavior.
- The amd64 and i386 sysroots are deterministic and are the sole public
  header/startup/libc/compiler-runtime boundary used by migrated userland.
- All maintained amd64, i386 PC/AT, and i386 PC-98 kernels and userland build
  with the project LLVM tools. Target Noct is included in that statement.
- All BIOS/UEFI loader and disk-image outputs build without a host target GCC,
  GNU binutils, or MinGW installation.
- The p006 final matrix reaches its declared runtime checkpoints, including
  target Noct non-JIT, JIT/RW-to-RX, and BeUI behavior on amd64 UEFI.

## Execution result

Q064 completed all seven Phases on 2026-09-02. The pinned LLVM 23.1.0 cache,
both x86 sysroots, every maintained x86 image, and the local CI sequence pass.
The final runtime evidence includes the uninterrupted six-cell amd64
firmware/Variant matrix, i386 PC/AT login, maintained PC-98 login/Xzed mouse,
and amd64 UEFI target-noct non-JIT, JIT/RW-to-RX, and BeUI acceptance.

## Exclusions and reconsideration boundaries

- Native Windows or macOS host bootstrapping is not promised by this first
  WS. The host must provide a suitable C++ compiler, CMake, Make, archive,
  checksum, patch, and download tools to bootstrap LLVM.
- LLVM libc, libc++, libunwind, LLDB, MLIR, Flang, sanitizers for zedBSD, and a
  general SDK/package distribution are outside the first implementation.
- sparcv9, m68030, arm64, and their bootloaders are not migrated here.
- Stop for human judgment if a stable zedBSD ABI cannot be represented without
  changing public headers, if the two x86 boards require incompatible i386
  sysroots, or if UEFI production requires a non-LLVM proprietary/runtime
  dependency. Ordinary Clang/GNU flag or assembly incompatibilities are
  implementation work, not new product decisions.

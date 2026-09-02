# Queue: reproducible x86 LLVM toolchain and sysroots

Last updated: 2026-09-02

QID: `q064`

Queue status: finished

Queue finished: **Yes**

Authorization: the user approved the complete WS021 plan, directed that
`userland/base/noct` use the project-built target toolchain, requested that
intermediate QEMU runs be minimized, and explicitly authorized placing the
work in a Queue and executing it.

Timebox: none. Execute the seven finite WS021 Phases below in dependency order.
P001--p005 and p007 use acquisition, unit, compile, link, image-format, and
dependency audits; p006 owns the consolidated runtime campaign.

Parent: [master plan](master.md)

Previous Queue: [q063](queue-q063.md)

## Purpose

Bootstrap verified LLVM 23.1.0 host tools below `build/llvm/`, construct the
amd64 and shared i386 sysroots, migrate all maintained x86 target code and
bootloaders away from host target GCC/binutils/MinGW, rebuild target Noct with
`x86_64-unknown-zedbsd`, and accept the result with one final six-cell QEMU
matrix.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws021-p001` | [LLVM source and zedBSD target](ws021-llvm-toolchain/phase001-llvm-source-and-zedbsd-target/phase.md) | complete (`q064`) | Verified LLVM 23.1.0 source intake and strict zedBSD triple/driver/compiler-rt patch |
| 2 | `ws021-p002` | [host LLVM build](ws021-llvm-toolchain/phase002-host-llvm-build/phase.md) | complete (`q064`) | Config-free `make toolchain` installs the required X86 Clang/LLD/LLVM tools below `build/llvm/` |
| 3 | `ws021-p007` | [host LLVM release asset](ws021-llvm-toolchain/phase007-host-llvm-release/phase.md) | complete (`q064`) | A validated x86_64 Linux host-tool archive is retained on `rev-0` and CI consumes its pinned digest |
| 4 | `ws021-p003` | [x86 sysroots](ws021-llvm-toolchain/phase003-x86-sysroots/phase.md) | complete (`q064`) | Deterministic amd64 and shared i386 public sysroots contain startup, libc, compiler runtime, and linker inputs |
| 5 | `ws021-p004` | [x86 target migration](ws021-llvm-toolchain/phase004-x86-target-migration/phase.md) | complete (`q064`) | All x86 kernels/userland, including target Noct, use the project LLVM tools and sysroots |
| 6 | `ws021-p005` | [bootloader closure](ws021-llvm-toolchain/phase005-bootloader-toolchain-closure/phase.md) | complete (`q064`) | BIOS/UEFI loaders and all x86 images build without host target GNU/MinGW tools |
| 7 | `ws021-p006` | [big-bang QEMU acceptance](ws021-llvm-toolchain/phase006-big-bang-qemu-acceptance/phase.md) | complete (`q064`) | The final six-cell amd64/i386/PC-98 runtime campaign passes, including target Noct non-JIT/JIT/BeUI |

## Accepted execution policy

- The host compiler builds host Noct and host LLVM only.
- zedBSD target artifacts use `build/llvm/`, explicit zedBSD triples, and the
  matching sysroot; silent PATH fallback is forbidden.
- `userland/base/noct` is a target artifact and therefore uses the new amd64
  LLVM/sysroot boundary. Its upstream BeUI implementation remains unchanged.
- Do not run intermediate QEMU as a Phase dependency. If p006 exposes a
  runtime defect, diagnose with the narrowest affected cell and rerun the full
  final matrix once after repair.
- sparcv9 and m68030 are outside q064 and retain their current path pending a
  later project-built GCC workstream.
- Do not consume `.internal/` and do not run aggregate `make check`.

## Completion definition

Q064 finishes when every item is either completed with recorded evidence or
honestly uncleared with facts and a concrete resume condition, P/W/M/Q records
agree, and no item remains pending or in progress.

## Execution result

Finished on 2026-09-02 with all seven items complete.

- Official LLVM 23.1.0 source plus the strict `zedbsd3` patch produces
  `x86_64-unknown-zedbsd` and `i386-unknown-zedbsd`; project Clang/LLD 23.1.0
  and both sysroots build without a host target-tool fallback.
- Release `rev-0` retains
  `zedbsd-llvm-23.1.0-x86_64-linux.tar.gz` (108322259 bytes), pinned at SHA-256
  `6f8e1154c73b9f2d32f16360ace107b7862f08e748c6f10c1bd75914aa6502c2`.
  `make toolchain-cache` validates it before the ordinary sysroot build.
- All four `config/ci/` builds pass in workflow order. BIOS and UEFI loader
  production uses Clang integrated assembly, LLD/`lld-link`, and LLVM binary
  tools; the PC/AT real-mode call/return encodings are explicit.
- The final six-cell amd64 firmware/Variant matrix passes. i386 PC/AT reaches
  login, maintained qemu-pc98 reaches login and passes the Xzed mouse oracle,
  and amd64 UEFI target noct passes non-JIT, JIT/RW-to-RX, and upstream BeUI
  graphics/evdev acceptance.
- The CI workflow downloads verified sources, installs the pinned cache,
  finishes `make toolchain`, builds all four images, publishes one artifact,
  and restricts nightly releases to pushes on `main`.

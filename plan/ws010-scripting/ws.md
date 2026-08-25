# WS010: Noct scripting and build tools

Last updated: 2026-08-25

WSID: `ws010`

Status: complete

Parent: [master plan](../master.md)

Last verified Phase: `ws010-p004`

Resume point: none; WS completion conditions are satisfied.

Shared tests: [WS010 tests](tests/README.md)

## Goals

- Build the upstream `awemorris/NoctLang` host interpreter through
  `make toolchain`, with its checkout and build artifacts below `build/`.
- Replace every Python script reached by the amd64, PC-98 i386, and PC/AT i386
  disk-image dependency graphs with a Noct script.
- Make Noct the supported scripting runtime for those image builds and their
  QEMU acceptance tooling.
- Preserve bootable amd64, PC-98 i386, and PC/AT i386 disk images.

## WS completion conditions

WS010 is complete when `make toolchain` checks out and builds Noct below
`build/`; none of the three required disk-image builds or their acceptance
harnesses invokes Python; all directly and transitively reached Python tools
have Noct replacements; and the amd64, PC-98 i386, and PC/AT i386 disk images
boot to the declared ready/login point in their emulators.

## Phase registry

| Combined ID | Phase | Status | Completion result |
| --- | --- | --- | --- |
| `ws010-p001` | [Noct host toolchain](phase001-toolchain/phase.md) | Complete | `make toolchain` obtains pinned Noct and passes the new WS010 smoke test |
| `ws010-p002` | [x86 build-tool migration](phase002-x86-build-tools/phase.md) | Complete | The frozen 15-script production closure is replaced by Noct |
| `ws010-p003` | [x86 dependency-closure audit](phase003-python-removal/phase.md) | Complete | Forced dry-runs show no Python on the three production paths |
| `ws010-p004` | [three-platform boot acceptance](phase004-boot-acceptance/phase.md) | Complete | amd64, PC/AT i386, and PC-98 i386 reach `login:` in QEMU |

## Scope

The migration covers only repository-owned Python directly or transitively
reached by the three required disk-image builds and boot harnesses. Other
platform, audit, menu, and asset-generation Python is outside this WS.
`.internal/` is excluded completely: it is neither an input nor a migration or
test source. Upstream files inside the ignored Noct checkout are also outside
zedBSD source ownership.

Tests required by this WS are implemented under `plan/ws010-scripting/tests/`
without copying from or depending on `.internal/`.

## Fixed decisions

- Checkout path: `build/NoctLang`.
- Host interpreter path: `build/NoctLang/build-static/noct`.
- Upstream repository: `https://github.com/awemorris/NoctLang.git`.
- Noct scripts use `.noct`; Makefiles invoke the built interpreter explicitly.
- New Makefile targets do not reintroduce a formal aggregate test interface.
- `.internal/` is never a required runtime/build dependency at WS completion.

## Interruption rule

At pause, record the last migrated script, remaining reachable Python inventory,
last passing tool contract, last booted platform, and next exact command in the
active Phase and this WS. A wrapper that merely invokes Python is not a migrated
script.

# WS016 Phase 003: `/sbin/swapon` and `/sbin/swapoff`

Last updated: 2026-08-28

WSID: `ws016`

Phase ID: `p003`

Combined ID: `ws016-p003`

Status: Complete (`q021`)

Parent: [WS016](../ws.md)

Tests: [WS016 test index](../tests/README.md)

## Objective

Add small native base-system commands that invoke the versioned runtime-swap
ioctls and install under `/sbin` on all configured platforms.

## Fixed command contract

```text
swapon [--] SOURCE...
swapoff [--] SOURCE...
```

- At least one source is required; no arguments or an unknown option prints a
  one-line usage message to standard error and exits 2.
- `--` ends option parsing so an otherwise option-looking operand is passed to
  the kernel.
- Operands are attempted left to right and processing continues after an
  operational error. Each failure names the command, exact operand, and
  `strerror` result on standard error.
- Exit status is 0 when all operands succeed and 1 when any operational request
  fails.
- Both commands open `/dev/system` themselves and rely on kernel privilege
  enforcement. They do not pre-parse or reinterpret UUID, PARTUUID, device,
  boot-slot, or file selectors.
- `-a`, `-s`/`--show`, priority options, formatting, and `fstab` integration are
  absent from version 1.

These are zedBSD extensions; POSIX/SUS does not specify either command.

## Work packages

1. Add separate base packages named `swapon` and `swapoff`, sharing a small
   local control helper where useful.
2. Use `PREFIX`-aware package installation with explicit `/sbin` destination
   and include the tools in the supported image manifests.
3. Add host command fixtures for argument parsing, ordering, diagnostics,
   kernel-error propagation, `--`, and aggregate exit status.
4. Apply the repository C format and warning policy to new source.

## Completion conditions

- SWAP-T009 and SWAP-T010 pass every CLI and mocked-ioctl case;
- production binaries link against the native base libc and pass ELF checks;
- both files appear executable as `/sbin/swapon` and `/sbin/swapoff` in the
  amd64 image;
- `make -j16` and `git diff --check` pass; and
- no external implementation, aggregate `make check`, or `.internal/`
  material is used.

## Reconsideration boundary

Stop if a requested selector cannot be represented by the p002 bounded UAPI.
Do not add an unplanned configuration parser or silently create/format swap.

## Execution result

Completed on 2026-08-28. The two independent base packages share only a small
header implementation, pass selectors unchanged through the fixed p002 UAPI,
and implement the exact multi-operand, `--`, diagnostic, continuation, and exit
status contract. Both native amd64 ELFs build and install as mode 0755 at
`/sbin/swapon` and `/sbin/swapoff`; PC/AT and PC-98 cross-ABI object builds also
pass.

SWAP-T009/T010 passed in strict and ASan/UBSan variants. `make -j16`, package
installation checks, native ELF checks, and `git diff --check` passed without
`make check`, `.internal/`, or an external implementation.

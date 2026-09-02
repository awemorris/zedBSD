# Q063 Noct 2.0.1 and source-lifecycle evidence

Date: 2026-09-02

Queue: `q063`

Phases: `ws010-p005`, `ws008-p010`, `ws008-p009`

## Source identity and ownership

- Release: `v2.0.1`
- Tag commit: `ed621e79139f55d06dd1a474243afbf0ce5efe0a`
- Archive size: `2524680`
- Archive SHA-256:
  `68588c84f508856474526be1c576cf6190ee99539cd81cc8453857d894f98f9f`
- Tracked owner: `userland/base/noct/`
- Ignored target extraction: `userland/base/noct/noct/`
- Ignored host extraction: `build/NoctLang/`

Both extractions come from the same verified archive. Extraction is staged in
a temporary directory, applies the tracked patch with zero fuzz, records a
source identity and file manifest, then publishes atomically. Archive size,
digest, member path and member type are all checked on temporary downloaded
bytes before the canonical cache is published. Re-running both source
verifiers after host and target builds passed.

The tracked patch is not a BeUI adapter. It changes only two upstream CMake
integration points: the required path of the zedBSD-owned final-link adapter,
and the conditional include/invocation which supplies crt0, libc, static-link
options, and the zedBSD linker script. The upstream
`src/api/api-beui-zedbsd.c` implementation remains unchanged and is the BeUI
implementation exercised below.

The previous dirty ignored checkout was preserved without modification at
`/home/awe/zedBSD-userland-noct-NoctLang-backup-20260902`; it is outside all
live build inputs. A clean earlier host checkout was separately retained below
the ignored `build/` tree.

## Lifecycle and host results

- `make download` passed without `config.mk`, and a second invocation reused
  and revalidated local bytes.
- Both firmware package cache suites passed after connection to the common
  lifecycle.
- `SCT-T043` passed seven network-free cases: wrong size, wrong digest, unsafe
  member path, unsafe member type, interrupted transfer, a valid control, and
  a fuzz-only patch rejection. Every rejected acquisition left no canonical
  archive/source, lock, reject file, or temporary path; strict-patch failure
  retained only its already verified archive cache.
- The lifecycle inventory found 177 maintained userland Makefiles and proved
  that each exposes `download`, `patch`, `build`, and `install`; config-free
  dry runs of both preparation stages passed.
- Standalone Noct `build` and `install` passed with an amd64 private config and
  installed byte-identical `noct` plus `holoris.nct` into a disposable stage.
- Removing only the generated host executable and retaining its build stamp
  caused `make -j16 toolchain` to rebuild it successfully.
- Host executable SHA-256:
  `db128557cacc7385976e491a26528bf14e3cfd47a2b9dbff78a63a64617653f6`
- `NOCT-T080`--`NOCT-T082` and `NOCT-T085` passed, including interpreter and
  compiled-application `--path` behavior and 73 live recipe consumers.
- `NOCT-T083`, `NOCT-T086`, clean/incremental `make -j16 toolchain`, and the
  ordinary configured `make -j16` passed.

## Target and QEMU results

The canonical `zedbsd-amd64` preset produced an ELF64 x86-64 static executable
with entry `_start` at `0x4066db`, no ELF interpreter, and no dynamic segment.
The CMake artifact, package artifact, and staged `/usr/bin/noct` are
byte-identical at SHA-256:

`e8ee34e05a79f89baefe30f57932cb7c543b4285edfddd61e9083e4e1ad92641`

Noct is optional and selectable only for amd64. It is absent from effective
PC-98/i386 selection. Remacs remains held; forcing the stale Remacs name no
longer retains Noct as an orphan dependency.

Three disposable q35/xHCI USB-root QEMU cells passed:

- `NOCT-T003`: installed-artifact identity and deterministic non-JIT command;
- `NOCT-T020`--`NOCT-T022`: direct RW-to-RX execution, forced JIT, and
  interpreter control;
- `NOCT-T011`--`NOCT-T013`: upstream BeUI drawing, Shift state, relative
  pointer motion, button input, close, and console return.

The first non-JIT attempt used legacy IDE and stopped before Noct at
`BIO_FLUSH` with the already recorded intermittent `BUG-001`. Re-running that
image succeeded, and the canonical WS008 QEMU gates were then moved to the
primary q35/xHCI USB-root path so the deferred IDE defect is not a Noct
acceptance dependency.

Raw logs and disposable images remain ignored under `build/q063-*`. No
physical hardware result is claimed by q063.

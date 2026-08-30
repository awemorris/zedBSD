# WS020 Phase 003: complete QEMU Variant matrix

Last updated: 2026-08-31

WSID: `ws020`

Phase ID: `p003`

Combined ID: `ws020-p003`

Status: Uncleared

Parent: [WS020](../ws.md)

## Objective

Prove the three generated profiles with their intended firmware and prove that
each single-firmware profile genuinely lacks the other boot path.

## Six-cell matrix

- `UEFI + BIOS (for PC/AT)`: SeaBIOS positive and OVMF positive.
- `BIOS (for PC/AT)`: SeaBIOS positive and OVMF negative.
- `UEFI (for Apple)`: OVMF positive and SeaBIOS negative.

Every cell uses a disposable writable image copy. OVMF cells use an independent
variables copy. The maintained runner records exact source/artifact/firmware
hashes, a shell-escaped QEMU command, guest debug output, serial and firmware
logs, elapsed time, and a machine-readable result.

Before UEFI-only boot, the runner proves the fixed 202,392,064-byte geometry,
pure Protective MBR, valid primary header/table, exact partition bounds, and
zero final 33 sectors. OVMF is allowed to leave that tail zero or repair it to
a complete valid backup GPT; any partial, malformed, contradictory, or
unrelated nonzero tail fails. The immutable source artifact must not change.

## Oracles

- Every positive cell reaches the configured `init: system running` and exact
  `login:` prompt, identifies the expected USB medium and root-overlay path,
  settles briefly, and contains no GPT, xHCI, USB-storage, VFS, panic, or fault
  diagnostic.
- The OVMF-negative BIOS image must show firmware discovery failure and entry
  into its fallback shell while exposing no zedBSD guest marker.
- The SeaBIOS-negative UEFI image must first pass the production byte checker,
  which proves an all-zero MBR bootstrap and no zedBSD BIOS loader. SeaBIOS
  must then select the disk and hand the pure Protective MBR to `0000:7c00`,
  remain alive through the bounded settle, stop cleanly under runner control,
  and expose an exactly empty zedBSD guest log. Neither the handoff marker nor
  a timeout is acceptance by itself.

## Verification and completion

One maintained Noct runner executes all six cells with
`qemu-system-x86_64`, Q35, xHCI, USB Mass Storage, single-thread TCG, and one
vCPU. SMP behavior is outside this image/firmware-path Phase. The focused
static layout gate, `make -j16`, and `git diff --check` also pass. No capacity
matrix, sparse materialization, or wrong-sized-medium test remains.

## Result

The maintained runner is
`plan/ws020-intel-mac/tests/qemu-variant-matrix.noct`. It owns a private build,
enables xHCI and USB Mass Storage explicitly, preserves each source hash, and
implements the six strict positive/negative oracles above. The byte-level
layout tests, GPT host ordinary/sanitizer/analyzer tests, three-Variant artifact
invariance test, `make -j16`, and `git diff --check` passed.

The full runtime matrix is **uncleared**. Three fresh attempts stopped at the
unchanged positive `login:` oracle:

- `temp/p003-fixed-20260831-agent-004`: both Hybrid cells passed; BIOS/SeaBIOS
  reached overlay root, swap, runtime filesystems, and `boot: starting init
  /sbin/init`, but emitted no `login:` before the 90-second bound.
- `temp/p003-fixed-20260831-agent-005`: both Hybrid cells and both BIOS cells
  passed; UEFI/OVMF reached the same `boot: starting init /sbin/init` point but
  emitted no `login:`.
- `temp/p003-fixed-20260831-agent-006`: Hybrid/SeaBIOS passed; Hybrid/OVMF
  started `getty_console`, emitted `init: system running`, but still emitted no
  `login:` before the bound.

The symptom crossed BIOS/UEFI firmware, image layout, four-vCPU MTTCG,
one-vCPU MTTCG, and one-vCPU single-thread TCG. The image, USB-storage, GPT,
overlay-root, and swap paths had already succeeded in every stopped cell. This
is therefore retained as a pre-existing runtime init/getty scheduling flake,
not converted into an image-layout success by retries or a weaker oracle.

Resume after that runtime issue has a diagnosis/fix in a separately authorized
Phase. Completion still requires one fresh, uninterrupted six-cell PASS with
the strict `login:` and post-login settle gates.

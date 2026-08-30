# WS020 Phase 003: complete QEMU variant and capacity matrix

Last updated: 2026-08-30

WSID: `ws020`

Phase ID: `p003`

Combined ID: `ws020-p003`

Status: Planned after `ws020-p002`

Parent: [WS020](../ws.md)

## Objective

Prove the generated profiles with the intended firmware and prove that the
non-selected firmware path is genuinely absent.

## Matrix

- Hybrid: one SeaBIOS boot and one OVMF boot to `login:`.
- BIOS-only: SeaBIOS reaches `login:`; OVMF finds no zedBSD boot path.
- UEFI-only: OVMF reaches `login:`; SeaBIOS finds no zedBSD boot path.
- For every 2/4/8/16/32/64/128/256-GiB selection, materialize a fresh sparse
  copy to that exact logical size, verify the primary header's last-LBA
  declaration and absent zero backup region, then perform at least the bounded
  OVMF discovery/boot check required by the UEFI-only profile.

All writable boots use disposable copies.  The runner retains exact image and
artifact hashes, logical/apparent sizes, allocated-block size, firmware, QEMU
command, boot log, and result.  It rejects a file copied into the wrong-sized
medium before claiming firmware acceptance.

## Verification and completion

The full matrix is one maintained finite runner using `qemu-system-x86_64`.
Every positive cell reaches the configured init/login without VFS/storage
errors, and every negative cell proves absence rather than timing out
ambiguously.  The 256-GiB case remains sparse on the host.  Normal amd64 UEFI
USB-root behavior, `make -j16`, and `git diff --check` pass.

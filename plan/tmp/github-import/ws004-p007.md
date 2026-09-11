<!-- awesome-plan project=zedbsd record=ws004-p007 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws004/phase007/phase.md`

親: [ws004](https://github.com/awemorris/zedBSD/issues/5)

# WS004 Phase 007: PC/AT warm-reset reinitialization

Last updated: 2026-08-25

Phase ID: `ws004-p007`

Status: complete

Acceptance disposition: **Cleared in QEMU**

Parent: [WS004](https://github.com/awemorris/zedBSD/issues/5)

Tests: [WS004 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws004-hardware/tests/README.md)

## Objective

Make `/sbin/reboot` complete a real second amd64 PC/AT boot through loader,
kernel, init, and `login:` without retaining allocator or other kernel BSS
state from the first boot.

## Baseline defect

The first reboot attempt exposed an unaligned signal FPU-save slot, which has
already been corrected. The loader and kernel are now entered again, but the
second kernel stops at:

```text
hal_set_allocator must be called exactly once
```

Allocator pointers expected to begin zeroed survive the warm-reset path. It is
not yet known whether the loader BSS clearing, reset mechanism, memory mapping,
or SMP shutdown/reset boundary owns the defect.

## Scope

- Reproduce after `ws004-p006` clears USB writes, with IDE as an independent
  storage control where useful.
- Instrument the earliest loader/kernel boundary using a bounded sentinel or
  equivalent evidence.
- Audit ELF `p_filesz`/`p_memsz` BSS clearing and the PC/AT reset mechanism.
- Audit AP shutdown/reset ordering so no old CPU continues executing during
  the next boot.
- Correct the owning reset or initialization path and remove temporary
  diagnostics that are not generally useful.

## Non-goals

- Hiding stale state by selectively resetting allocator globals.
- ACPI power-management expansion beyond what a correct reboot path requires.
- Suspend/resume or hibernation.

## Ordered work packages

- [x] Reproduce and record the last first-boot and first second-boot markers.
- [x] Prove whether the loader clears the physical BSS range actually used by
      the second kernel.
- [x] Prove CPU/reset ownership and identify the first stale write/state.
- [x] Implement the bounded architectural fix.
- [x] Boot, login, reboot, and reach a second login repeatedly under QEMU.
- [x] Run configured builds, `make -j16`, and `git diff --check` without
      `make check`.

## Completion conditions

- At least three consecutive guest-requested reboots reach a fresh `login:`.
- Kernel BSS sentinels and allocator state begin clean on every boot.
- The result passes with the production amd64 BIOS image; UEFI is also checked
  if its available reset path exercises the same kernel mechanism.
- No CPU continues executing old-kernel code across reset, and reset waits are
  bounded.
- Focused tests, `make -j16`, and `git diff --check` pass.

## Result

The defect reproduced with both one and two virtual CPUs, excluding AP-only
state as its cause. On the second BIOS load, the ELF program header still held
the correct `p_filesz=0x12160` and `p_memsz=0x339000`, but physical BSS from
`0x289000` onward retained first-kernel state. This included the allocator
function pointers at physical `0x43b008`/`0x43b010`.

The amd64 BIOS loaders no longer depend on unreal-mode hidden segment state for
ELF64 NOBITS initialization. Once the bounded one-GiB identity map is active,
their 64-bit entry code walks the validated PT_LOAD records and clears each
`p_memsz - p_filesz` range immediately before jumping to the kernel. The
program-header address is formed explicitly with `offset` plus the loader base;
Intel-syntax memory/immediate ambiguity is therefore avoided. ELF32 retains a
bounded, interrupt-protected chunked zero-fill path.

With temporary sentinels removed, a production SMP=2 BIOS/IDE image completed
three consecutive guest-requested reboots and reached four fresh `login:`
prompts without a fatal or disk error. The same production image also completed
a guest-requested reboot through q35/xHCI USB storage and reached the second
`login:`. Detailed evidence is in
[qemu-warm-reset-evidence.md](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws004-hardware/tests/qemu-warm-reset-evidence.md).

# WS048 p012 source/build evidence / 2026-10-11

Base: main `9a0ddc6cb`; private branch `codex/rpi4-usb-freeze` in `.claude/worktrees/rpi4-sshd`. User freeze photo ends at `boot: starting init /sbin/init` then `xhci: port 1 reset complete portsc=40000e03`. The later user clarification confirms boot is frozen and SSH has not started. Earlier SSH read-only connection timed out; further attempts suspended.

## Findings and change

Production command_ex previously disabled local IRQ across as many as 10,000,000 hardware polls. This is a verified progress limitation for a single-CPU RPi4, not evidence that it alone caused this specific physical freeze. Command completion now has a five-second hardware-counter deadline independent of IRQ/timer delivery; a missing counter uses scheduler ticks with the retained finite polling guard. Lost or changed counter frequency leaves the command uncompleted. The caller's original IRQ permission is restored between protected event-ring polls, with scheduling opportunities between them. Command admission, event locking, completion-pointer matching, delayed callbacks, command_failed and controller-owned DMA retention remain intact.

RPi4 Enable Slot diagnostics surround existing doorbell write/readback:

```
xhci: enable slot doorbell begin command=...
xhci: enable slot doorbell complete
```

A begin without complete identifies a publication/access stop. Both stages with command timeout identifies missing completion/progress. No stage is insufficient to infer a specific cause. No unsupported assumption is made about the firmware PCI DMA base; retain the earlier DMA alias and posted-write-completion fix until physical evidence contradicts it.

Linux's default xHCI command timeout is 5000ms: [primary source](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/host/xhci.h). No claim of identical Linux integration or of successful hardware recovery.

## Checks

- `python3 plan/ws048/tests/command-wait-host.py`: PASS, nine scenarios. Runner extracts the final production command_ex unchanged; the fixture models clocks, IRQ state, event-lock discipline and descriptor identity. It asserts completion by poll/IRQ handoff, stale event rejection, missing-event timeout, IRQ-disabled hardware-clock timeout, scheduler fallback, changed clock refusal, hardware refusal and already-failed command admission. Verifies gate release, required IRQ restoration, scheduling opportunities, finite polls and no new publication on a poisoned controller. Models do not exercise PCI transport or real DMA visibility.
- `make -j16 build/arm64/vmunix`: exit0; warnings0/errors0; arm64 ELF/raw-image validation PASS, entry `0xffff000000080000`. Exact configured driver set retained. Only kernel/raw image built; no full SD image or userland rebuild.
- `git diff --check`: PASS.
- clang-format19 on changed function range and new C fixture, followed by project canonical definition/one-line forward-declaration corrections and manual full changed-source review against `plan/coding-style.md`, Guardrail, AGENTS and `.clang-format`. ANSI declarations, explicit outcomes, comments/paragraphs, lock/IRQ/gate ownership and public/static order reviewed. Existing unchanged legacy source not mass-formatted. No test-only production environment controls.

Final hashes:

| Artifact | SHA256 |
| --- | --- |
| `src/drivers/pci/pci-xhci.c` | `743efabc24572aa57fd1cb54c0f7f8bff169ca7e8744f68c533652dd7668b55e` |
| `build/arm64/vmunix` | `d31c2a6498b7237a5827fc1afe562fe8695c82f3b813fa98f9f97849c4090080` |
| `build/arm64/kernel.elf` | `b4525635d65d2ba70c84fe30a1b2ecef6f0a9b5b935932072614e1523a0a44a4` |

## Outcome / residual work

Limited p012 source/build criteria cleared; WS048 remains incomplete. USB recovery, login, input and SSH startup are unverified. A blocked MMIO access or non-returning IRQ handler cannot be recovered by this polling deadline. Next physical boot should show the DMA window line, Enable Slot stages and any timeout/controller diagnostic, allowing the next repair to target observed hardware behavior. No automatic reinvestigation beyond this finite attempt.

Concrete-commit main integration needs Q1 boundary approval. Pending boot0 and CI commits remain on their separate branches. No deployment, reboot, push, toolchain change, QEMU or aggregate test. Shared Master/Queue/history/cache and GitHub projections remain Q1 pending.

# Codex Queue: rpi4-command-freeze-20261011

Status: finished
Owner: Codex / codex/rpi4-usb-freeze
Approval: current user「USBドライバがフリーズして起動できなくなりました」with photo ending after port1 reset. Continuing the user's USB functional repair request.
Finite scope: bound command completion waits by an interrupt-independent counter, restore IRQ delivery and scheduling between protected polls when the caller permits it, retain event/command/DMA ownership and failed-command refusal, add first Enable Slot doorbell stage diagnostics. Focused production-function host scenarios and warning0 arm64 kernel build/full changed-source C review. No MSI, GPU, boot0/CI integration, toolchain, remote update/reboot/push or shared-plan edits.

| Attempt | Phase | Status | Dependency |
| --- | --- | --- | --- |
| rpi4-command-freeze-20261011-i01 | [ws048-p012](phase012/phase.md) | cleared（source/host/build） | main9a0ddc6cb; user freeze photo |

Graph: existing DMA/doorbell source (context) → i01 → physical USB/boot acceptance (context).
SSH read-only attempt times out. Exact hardware stopping point and newly logged PCI DMA base remain unverified. Shared projections/GitHub Q1 pending; no automatic next Queue.

Outcome: [p012 evidence](tests/command-freeze-20261011.md); production waiter 9 scenarios PASS, current config warning0 arm64 kernel/image checks PASS, final source standards review/diff-check PASS. User confirms SSH has not started because boot freezes; further SSH attempts suspended. Physical USB/boot acceptance and main integration remain pending, with prior source/build history preserved. No next Queue or remote effect started.

## Main integration / 2026-10-11

Current user「mainにマージしてください。」approves the exact USB repair commit `17898a7465b9125bb5f4afef9539cdeb66e9b162`. Clean main `9a0ddc6cb` fast-forwarded to this commit without conflict. Read-back confirms all seven integrated files byte-identical to the verified private worktree; config.mk matches the warning0 arm64 build configuration and xHCI SHA256 matches the recorded final source hash. Previous nine host scenarios and kernel/raw-image validation apply to this identical source; no redundant rebuild/test. Source integration is complete; earlier integration-pending statements are historical. Physical boot/USB recovery remains unverified and WS048 stays incomplete. Pending boot0/CI commits remain separate. No push or remote update/reboot. Shared Master/Queue/history/cache/GitHub projections remain Q1 pending.

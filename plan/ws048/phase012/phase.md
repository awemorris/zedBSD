<!-- awesome-plan project=zedbsd record=ws048-p012 -->
# WS048 p012: bounded command waits and CPU progress

Status: cleared（限定source/build criteria、実機復旧未確認）
Disposition: normal
Parent: [WS048](../ws.md)
Queue: [rpi4-command-freeze-20261011](../codex-command-freeze-20261011.md)

User photo ends at port1 reset after starting init; SSH times out. No measured wait duration, complete new DMA log or exact hardware-stage read-back is available. Source has an interrupt-disabled ten-million-iteration command poll, which can starve the single RPi4 CPU's timer/init/network. This is a verified software progress limitation, not proof of the physical freeze's root cause.

Design: time-bound command completion to five seconds using the existing HAL-backed monotonic counter (no HAL API change), with scheduler tick/iteration fallback. Keep command admission exclusive. Disable IRQ only around event-ring/command state shared with IRQ; allow IRQ delivery and yielding between polls for callers that originally enabled IRQ. Preserve IRQ-disabled callers, DMA retention and command_failed after an uncompleted command. Identify whether first Enable Slot doorbell publication returns using two board-specific diagnostic stages. Do not blindly remove required PCI posted-write completion or change DMA aliases without evidence.

Criteria: actual production command_ex function host fixtures for poll and IRQ completion, timeout with missing events, IRQ-disabled caller, clock fault, stale completion and failed-command admission. Current config warning0 arm64 kernel/image build, full final changed-source C review, diff-check. Actual hardware/boot acceptance remains in p009/p006 and is not supplied by host tests. Instructions: AGENTS/Guardrail/full coding-style.md/.clang-format; private worktree/build and read-only toolchain. No GPU, boot0, CI, remote deployment/reboot/push or shared Master/cache changes. Main integration is separate concrete-commit approval; Q1 projections pending.

## Outcome / 2026-10-11

限定source/build attempt cleared。[証拠](../tests/command-freeze-20261011.md)。Actual production command_ex host modelの9 scenarios PASS、warning0 arm64 kernel/raw vmunix checks PASS、最終変更Cの全文規約review/formatter+canonical signature補正/diff-check PASS。5秒deadlineはIRQ-offでも進む既存HAL hardware counterを使い、許可callerのIRQ/CPU進行を保護区間外で確保。clock fault/missing event時のcommand_failedとDMA retentionを保持。

User clarified「そもそもフリーズしてSSHは起動してないです」; SSH probing is suspended. Exact physical root cause remains unverified. A stuck PCI MMIO access or an interrupt handler that does not return cannot be rescued by this command-loop deadline. Next boot stages distinguish whether Enable Slot doorbell publication returns; user physical acceptance and exact main integration remain pending. This limited clearance does not clear p009/p006 or complete WS048. No boot0/CI commits, deployment/reboot/push, shared plans/cache or toolchain changes.

## Main integration / 2026-10-11

Current user「mainにマージしてください。」approves the exact USB repair commit `17898a7465b9125bb5f4afef9539cdeb66e9b162`. Clean main `9a0ddc6cb` fast-forwarded to this commit without conflict. Read-back confirms all seven integrated files byte-identical to the verified private worktree; config.mk matches the warning0 arm64 build configuration and xHCI SHA256 matches the recorded final source hash. Previous nine host scenarios and kernel/raw-image validation apply to this identical source; no redundant rebuild/test. Source integration is complete; earlier integration-pending statements are historical. Physical boot/USB recovery remains unverified and WS048 stays incomplete. Pending boot0/CI commits remain separate. No push or remote update/reboot. Shared Master/Queue/history/cache/GitHub projections remain Q1 pending.

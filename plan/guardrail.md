# Guardrail

Current project contribution rules, adopted 2026-09-10. User decisions prevail
when more recent or more specific; archived approvals are not standing authority.

## Scope and architecture

- Work only within the approved Phase scope. Kernel src/, include/, bootloader/,
  libc/, userland/, platform/, build/config and tools have different ownership;
  inspect current code and Phase before adding files or changing module boundaries.
- `include/hal/hal.h` and HAL responsibilities require explicit applicable user
  approval. The old VM ownership move, vmap removal plan and pmem argument expansion
  were superseded by the user's rollback/refactor. Do not replay them. Existing
  `hal_space_*` and shared kernel mapping support are not permission to redesign HAL.
- Keep the manually reviewed driver organization and `drv_` global-symbol policy.
  Trust verified refactoring over stale test assumptions; repair tests as appropriate.
- RTL8822B `.inc` files separate licensing and MUST remain separate. The earlier
  general instruction to merge `.inc` does not apply to these files.
- Base-system implementation/licensing boundaries: [design policy](master-design-policy.md).
  That document also contains historical milestones; current Master and source
  supersede outdated installer/HAL implementation descriptions.
- Do not copy kernel implementation into userland build dependencies. mkfs tools
  must remain independently usable; preserve the user-authorized source-copy split.

## Standards and verification

Full C standard: [coding-style.md](coding-style.md). Use the full applicable
sections before code generation; no unverified condensed substitute is installed.
The user's WS025 instruction allowed approximate style adherence followed by
later cleanup; preserve that scoped exception, not as a blanket exemption for
new WSs. Preserve evaluation order, ownership and behavior in style changes.
Tool coverage/versions: [automation](standards/automation.md).

- No `git commit` or push without user instruction; keep unrelated modifications.
- Do not read repository `.internal/`. No credentials in plans, Issues or sync state.
- Never run aggregate `make check`. Use meaningful focused checks for the Phase.
- Supported build gate: `make -j16` with the selected platform configuration.
  amd64 runtime uses `qemu-system-x86_64`; destructive tests use disposable images.
- User acceptance scope prevails: do not reintroduce rejected exhaustive abnormal
  tests, repeated physical boots, or hardware gates already waived by the user.
  QEMU and physical evidence remain distinct; describe precisely what was observed.
- Explain only actual external approval blockers. Existing exact-scope authorization
  persists; this adoption does not add a confirmation step for routine plan updates.

## Decision sources

User thread: HAL responsibility/hal.h restrictions and subsequent rollback review;
RTL8822B licensing exception; refactor/test trust; no commit; WS025 style flexibility;
normal-path installer acceptance and QEMU-only UAS acceptance. Retained source
records: [post-rollback review](ws025/post-rollback-review.md),
[WS025](ws025/ws.md), [WS019](ws019/ws.md). Older MWP-Q instructions are in old/.
New rules must update this registry, applicable full standard, tool coverage and
impacted plans; do not silently replace agreed architecture or scope.

## WSの単一目標と終了後の扱い（2026-09-12ユーザー指示）

WSは一つの具体的な到達目標を持つ。目標を達成したWS、またはユーザーが終了したWSは再利用・再開して別の目標を追加しない。似た領域だからという理由で一つのWSへまとめない。機種対応などの上位分類・到達点はMGが担い、インストーラ実機動作、PowerPC移植などは別のWSを作る。
一つの目標に必要な依存作業をPhaseへ分解することは可能だが、独立した別目標をPhaseとして混ぜない。WS終了時は子Phaseを全件照合し、未完了は完了に改変せず、ユーザー指定の保留先または別WSへ引き継いで元Phaseを終了する。旧ID、結果、転送先を残す。今回WS003は終了・再利用禁止、PPC移植はWS027へ、その他の未完了はFuture Workへ移す。

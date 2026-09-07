# Queue q122: WS025 integration acceptance and defaults

Date: 2026-09-08 JST
Status: finished
Authorization: standing user instruction to run autonomously through WS025 completion.
Timebox: review progress and evidence every 90 active minutes.
Previous: [q121](queue-q121.md), p025 completed with ordinary artifact restored.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p026](ws025-io-memory-cache/phase026-integration-defaults/phase.md) | completed | Freeze and verify cross-feature acceptance, select effective defaults, remove obsolete paths if any, and record conditional adoption decisions. Depends on completed mandatory p005–p025 and WS024. |

Execute [integration design](ws025-io-memory-cache/phase026-integration-defaults/integration-design.md).
Conditional p027–p030 implementation is not automatically included: their explicit
adoption decisions belong to this queue; absent evidence keeps them planned.
Physical acceptance is user-accepted, not agent-measured. No commits, aggregate
make check or .internal access. Serialize all builds/tests and use make -j16.

Result: p026 and mandatory WS025 p001–p026 completed. [Final acceptance](ws025-io-memory-cache/phase026-integration-defaults/results.md). Conditional p027–p030 explicitly not adopted; planned resume criteria recorded. All runs terminal; ordinary artifacts restored.

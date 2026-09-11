# Queue q121: storage recovery and media admission

Date: 2026-09-08 JST
Status: finished
Authorization: standing user instruction to run autonomously through WS025 completion.
Timebox: review implementation and evidence every 90 active minutes.
Previous: [q120](queue-q120.md), p024 completed with ordinary artifact restored.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p025](../ws025/phase025/phase.md) | completed | Join bounded SCSI recovery and media identity with common disk/cache admission, independent retirement, and checked shutdown. Depends on completed p001/p014 and preserves p024 staging ownership. |

Follow [recovery design](../ws025/phase025/recovery-design.md).
The key uncertainty is how common old-owner admission and idle replacement fit
existing disk/cache/loop lifetimes; resolve with production-source fault tests.
Do not drain the active BIO from its recovery path or reconnect a mounted object
to a different medium. Native physical acceptance remains user-accepted, not
agent-measured. Serialize builds/tests and restore link-only probes explicitly.
No commits, aggregate make check or .internal access. Use make -j16.

Result: p025 completed; [evidence](../ws025/phase025/results.md). All runs terminal, ordinary artifact restored. Continue with p026.

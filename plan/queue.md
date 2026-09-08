# Queue q123: WS025 driver layout and coding style

Date: 2026-09-08 JST
Status: in-progress
Authorization: user explicitly instructed execution of the agreed p031 plan.
Timebox: review progress and evidence every 90 active minutes, as in q122.
Previous: [q122](queue-q122.md), finished.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p031](ws025-io-memory-cache/phase031-driver-layout-style/phase.md) | in-progress | Apply agreed driver layout, symbol naming/style, independent mkfs and command packaging; depends on completed p026. |

p027–p030 remain outside this Queue. Consolidation can expose private-name collisions
and test source-extraction dependencies; preserve production behavior and test coverage.
No commits, aggregate make check or .internal access. Serialize builds/tests/runtime;
use make -j16. Current user execution authorization applies to this finite scope.

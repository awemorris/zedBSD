# Queue q123: WS025 driver layout and coding style

Date: 2026-09-08 JST
Status: finished
Authorization: user explicitly instructed execution of the agreed p031 plan.
Timebox: review progress and evidence every 90 active minutes, as in q122.
Previous: [q122](queue-q122.md), finished.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p031](ws025-io-memory-cache/phase031-driver-layout-style/phase.md) | uncleared | Apply agreed driver layout, symbol naming/style, independent mkfs and command packaging; depends on completed p026. |

p027–p030 remain outside this Queue. Consolidation can expose private-name collisions
and test source-extraction dependencies; preserve production behavior and test coverage.
No commits, aggregate make check or .internal access. Serialize builds/tests/runtime;
use make -j16. Current user execution authorization applies to this finite scope.

2026-09-09: q124 へ優先順位を変更。p031 はその後の Claude による再編で旧検証証拠が現行ツリーを覆わないため uncleared。現行ツリーのデグレを p032/p033 で先に修正し、p031 の完了判定は残件として維持する。

# ws018-p017: Storage baseline and image audit

Date: 2026-09-06
Status: completed
Queue: q086
Authorization: user's explicit plan/50 scenarios/queue/execute request.
Scope: Record source, expose experimental controls, inspect current image directory sizes; compare four operation-count cells and clearly identify hardware-only measurements.

Design and exclusions: [shared implementation plan](../../old/fs-report-implementation-1.md).
Acceptance: [50 stories](../../old/fs-acceptance-50.md).
Dependencies follow queue order. Results must distinguish host doubles, actual
production code, native QEMU and physical hardware. Save command/log evidence and
remaining work in results.md. No commit, no make check, make -j16 serialized.
Completion requires implemented scope and its applicable acceptance gates;
unrun native checks are not passes.


Evidence: [results](results.md).

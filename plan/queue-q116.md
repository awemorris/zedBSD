# Queue q116: UFS deferred metadata acceptance

Date: 2026-09-07
Status: finished
Authorization: standing user approval for autonomous WS025 completion.
Timebox: review every 90 active minutes; start 2026-09-07 14:03 UTC.
Previous: [q115](queue-q115.md), finished with full p021 uncleared.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p021](ws025-io-memory-cache/phase021-ufs-metadata-writeback/phase.md) | completed | Broaden deferred allocation/truncate/namespace fault injection, close CRASH/META/WB/FLUSH coverage, and run FS50/Wi-Fi30/native storage regression. Depends on q111–q115 verified ownership and opt-in checkpoint integration. |

Use finite existing production-linked fault families with a deferred-policy test
mode, preserving synchronous cases and real-core recovery. Fix demonstrated
contract failures within p021, and record any unsupported geometry or ownership
boundary explicitly. Review acceptance mappings against older evidence and new
source changes; do not substitute assertion counts for required scenarios.
Then run supported/build and native gates appropriate to actual further changes.
No commits, aggregate make check, .internal access or concurrent build/runtime;
use make -j16. Physical gate remains user-accepted, not agent-measured.

Outcome: p021 completed. Deferred fault families, CRASH/META/WB/FLUSH mapping,
FS50/Wi-Fi30/native and combined USB data+metadata policy pass. See final phase results.

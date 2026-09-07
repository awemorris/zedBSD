# Queue q099: WS025 flush generations and completion ownership

Date: 2026-09-07
Status: finished
Authorization: user's autonomous WS025 completion instruction covers successive finite Queues.
Timebox: review every 90 active minutes; record facts and resume autonomously.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p014](ws025-io-memory-cache/phase014-flush-generations/phase.md) | completed | Track accepted/completed/stable writes, preserve upper drain and merge only proven repeated physical flushes. Requires completed p001. |

Implement leaf write sequence/frontier tracking with embedded outstanding BIO
links, no completion-record allocation. Finish leaf bookkeeping before terminal
BIO publication. Capture a flush target, wait for its outstanding writes, serialize
flush ownership, and publish only that target in the same invalidation epoch.
Errors/short writes/reset/media removal invalidate proof; saturated generations
never authorize elision. Eligibility is an explicit physical-driver persistence
capability; stacked/unknown devices keep forwarding every flush. USB recovery
invalidates prior proofs. Enable other drivers only with their reset contract
covered, otherwise retain ordinary flush behavior.

Upper logical dirty generations are separate from leaf counters. Add conservative
owner hooks and retain every existing FS/overlay/journal drain; mount sync records
only its captured logical target after successful full sync. Upper state never
uses leaf stability to skip its work. Test out-of-order completion, overlapping
flush/write, completion-storage lifetime, errors/reset, upper dirty-before-BIO,
and distinct overlay commit boundaries, then supported builds and native readback.

Previous: [q098](queue-q098.md). Physical gate remains user-accepted;
no agent physical runtime claimed. No commit, no aggregate make check;
serialize build/runtime. Production owners include disk/buf, mount/file, FAT/UFS,
overlay and driver capability/recovery hooks; use existing maintained fixtures.

Result: [p014 results](ws025-io-memory-cache/phase014-flush-generations/results.md); all selected gates passed.

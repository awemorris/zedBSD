# Queue q266: input-view operation counts

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution
Timebox: 60 active minutes
Previous: [q265](queue-q265.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](../ws025/phase028/phase.md) | uncleared | Clock-free link-only HAL operation counters; native input workload; restoration |

Counters aggregate external calls after input begins; not timing or exact syscall isolation.

Result: native input oracle PASS; 2048 leases show mean 9 SYS map calls per
64 KiB view, one user protection and one SYS unmap. Ordinary restored/no wrappers.
Counts identify cost candidates, not time attribution or adoption evidence.

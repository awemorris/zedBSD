# Queue q265: input-view cost attribution

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution
Timebox: 60 active minutes
Previous: [q264](queue-q264.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p028](ws025-io-memory-cache/phase028-direct-user-io/phase.md) | uncleared | Link-only lease/map profiling with existing monotonic counter, native input check, ordinary restoration |

No protection/lifetime relaxation or public HAL additions. Timing includes probe overhead.

Result: profile build passes; guest HAL counter unavailable, so timing prerequisite
fails and no stage data obtained. Ordinary build restored/no wrappers. Resume
with verified clock capability or count-only instrumentation.

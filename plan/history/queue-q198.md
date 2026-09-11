# Queue q198: xHCI stream ring ownership

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 120 active minutes
Previous: [q197](queue-q197.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](../ws025/phase029/phase.md) | uncleared | Implement xHCI primary stream rings, request provenance, dequeue/restart/free ownership and explicit gated USB endpoint configuration; focused checks/builds |

Keep one active request per endpoint. UAS SuperSpeed coordination and native
stream acceptance follow this host-controller step. Retain all context/ring DMA
on uncertain configuration; do not advertise usable UAS merely from compilation.

Result: stream-ring/configuration ownership implemented with focused actual-core
and xHCI tests; three builds and default-ring BOT/UAS lifecycle QEMU pass. HCD
capability stays unadvertised until SuperSpeed UAS integration/native testing.
Full p029 remains uncleared.

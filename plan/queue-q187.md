# Queue q187: multiple NVMe controllers

Date: 2026-09-10
Status: finished
Authorization: standing user approval for autonomous Priority execution;
explicit multiple-NVMe requirement after installer completion
Timebox: 120 active minutes
Previous: [q186](queue-q186.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws004-p050](ws004-hardware/phase050-nvme-multiple-controllers/phase.md) | completed | Replace controller singleton and hard-coded namespace controller index; preserve per-controller lifecycle; QEMU discovery and installed boot in both enumeration orders |

WS019 is complete including PC98 target-only login. No user decision blocks
this queue. Activate on execution; no new approval question is needed under the
standing user instruction. If full phase acceptance cannot finish this cycle,
record the achieved behavior and exact residual gates as uncleared and select
the next executable goal work. Do not silently move the required NVMe task to
Future. Detailed ownership design and acceptance remain in the P book.

Result: ws004-p050 completed. See P-book results for the QEMU/host evidence
and model boundaries. All three image builds and diff whitespace checks pass.

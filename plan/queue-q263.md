# Queue q263: native high DMA under explicit test capability

Date: 2026-09-10
Status: finished
Authorization: standing Priority execution / approved HAL consolidation
Timebox: 90 active minutes
Previous: [q262](queue-q262.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p037](ws025-io-memory-cache/phase037-hal-interface-consolidation/phase.md) | uncleared | QEMU-only high-capability HCD DMA owner, native fragmented >4 GiB transfer, restore ordinary build |

QEMU xHCI AC64 already observed in q254; verify again in new guest log. Test-only
owner lives with HCD for guest lifetime. Production PCI mask remains 32-bit.

Result: test-only high owner builds; two high vectors observed, then 16-segment
assert fails before full native acceptance. Ordinary build restored/no wrappers.
Failure evidence and diagnostic resume condition retained; all jobs terminal.

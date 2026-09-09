# Queue q160: installed NVMe boot acceptance

Date: 2026-09-09
Status: finished
Authorization: standing autonomous Priority instruction; queue presented before execution
Timebox: 120 active minutes
Previous: [q159](queue-q159.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p005](ws019-installation/phase005-qemu-nvme-overlay-install/phase.md) | completed | NVMe-only boot/persistence, selection and 12 fault-model runs; q160 consolidated acceptance passed |

Execute the [q160 design](ws019-installation/phase005-qemu-nvme-overlay-install/q160-design.md).
p004 is accepted by public6 cancel/install/rerun and conflict2 terminal PASS.
Use disposable copies of the accepted target; preserve the source and original.
No physical writes. Record defects and bounded resume work if a required cell
cannot be completed. p006/p007/p027/p028 remain planned subsequent queues.

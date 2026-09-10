# Queue q216: UAS replacement partition discovery

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q215](queue-q215.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](ws025-io-memory-cache/phase029-uas/phase.md) | uncleared | Reload partitions after removable medium publication outside command lock; host ordering/retry checks and supported builds |

Native partition-bearing medium insertion/exchange follows the implementation gate.

Result: partition reload implemented with balanced administrative open outside
command mutex. Host checks and HS/SS partition-bearing medium exchange pass;
three build commands exit 0. Initial EBUSY failure and PC98 staging warning are
recorded in phase results. Full p029 remains uncleared for removable transport
recovery and residual acceptance audit.

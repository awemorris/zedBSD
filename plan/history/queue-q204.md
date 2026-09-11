# Queue q204: UAS in-flight disconnect

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q203](queue-q203.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](../ws025/phase029/phase.md) | uncleared | Trace-proven pending UAS read, physical device removal, caller termination, replug/readback and halt |

Result: pending-read unplug reproduced. Direct QEMU device deletion asserts;
USB attached=false avoids it, but native driver teardown remains pending after
read termination. Physical-teardown media revocation implemented with host/build
checks; q204-super3 proves it is not a complete fix. Next identify retained owner.

# Queue q214: native in-place UAS medium exchange

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q213](queue-q213.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](../ws025/phase029/phase.md) | uncleared | Keep USB/SCSI device attached, eject old backing and insert distinct backing, require fresh publication and readback at both speeds |

Preserve initial backing and boot image; do not substitute physical unplug for
SCSI media exchange. Record failure and fix within this medium-lifecycle scope.

Result: both SS and HS in-place eject/change-medium pass with new-pattern readback.
Protocol audits prove absence then change, two disk publications and one USB class
binding. Kernel unchanged from q213. Full p029 remains uncleared; see phase results
for empty-LUN, partition rediscovery and stopped-removable-transport follow-up.

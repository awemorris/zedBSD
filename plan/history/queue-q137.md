# Queue q137: legacy USB-root recovery

Date: 2026-09-09
Status: finished
Authorization: User-authorized autonomous Priority goal; explicit QEMU legacy USB correction request.
Timebox: 120 active minutes
Previous: [q136](queue-q136.md), p021 uncleared with current runtime/accounting evidence.

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws006-p010](../ws006/phase010/phase.md) | uncleared | Paired EHCI/UHCI root ENODEV reproduction, proved correction, driver/input/root-I/O/halt regression |

Xzed GUI acceptance remains p009 after this prerequisite. No hardware gate.

Result: boot context corrected; paired dirty halt/reboot and xHCI pass.
Paired concurrent I/O stalls in a process-reaper heap walk; first-failure capture
continues in WS002-p024. See the p010 progress book for evidence and boundaries.

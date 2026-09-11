# Queue q228: public revoked UFS unmount

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 90 active minutes
Previous: [q227](queue-q227.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](../ws025/phase029/phase.md) | uncleared | Integrate closed mount admission/preflight/commit, expose umount -f through root-only syscall, build and run mounted-medium QEMU acceptance |

Result: public revoked UFS unmount and VM global-reclaim admission implemented.
Final SuperSpeed normal/refusal/force/republication scenario passes, focused VM
ordinary/sanitized checks pass, and three final builds pass. Full p029 remains
uncleared: dirty-retained acceptance and High-Speed BUG-022 follow-up remain.
High-Speed attempt stopped in mkfs before force; evidence is retained in results.

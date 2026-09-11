# Queue q141: capture and correct the UHCI heap writer

Date: 2026-09-09
Status: finished
Authorization: Standing autonomous Priority goal and explicit Daybreak
delegation for p024. Parent checks progress every 180 seconds.
Timebox: 90 active minutes
Previous: [q140](queue-q140.md), first structural failure captured, p024 uncleared.

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws002-p024](../ws002/phase024/phase.md) | uncleared | Daybreak proved and corrected the UHCI stale-link heap writer; the independent missing-login owner-count regression remains red |

One child owns source/build/runtime. Parent owns Queue/M/W and may do read-only
review or independent planning. Scope includes relevant UHCI/DMA ownership and
maintained fixture repairs needed to test it, not an unrelated USB rewrite.
At most three bounded capture attempts before reassessment; a proved defect
then receives targeted regression and original paired/xHCI validation.
No exact cause means uncleared with evidence, not a speculative fix.

Result: q141 proved the exact UHCI writer and stale schedule ownership, added a
failing-before/passing-after middle-unlink regression, and passed all heap/USB
gates. p024 remains uncleared solely because the required PCAT missing-login
lifecycle gate reports task/thread owners 9 to 8 without a UHCI or heap failure.
No later Phase was started, per the user's model-change stop instruction.

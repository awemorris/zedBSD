# Queue q183: native UFS installer final acceptance

Date: 2026-09-10
Status: finished
Authorization: standing autonomous installer completion instructions
Timebox: 120 active minutes
Previous: [q182](queue-q182.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p007](../ws019/phase007/phase.md) | completed | Actual page-out/page-in through installed UFS swap, fragmented-file admission, lifecycle and final evidence consolidation |

Use disposable clones of q182-native5, which was produced by the actual public
installer. Do not reprovision a target on the host and call that installer
acceptance. Reuse the existing WS016 pressure worker through a test-only probe;
no new installed helper command. Preserve the accepted image and hash it before/
after. If a kernel defect prevents completion, investigate/fix within this finite
phase if reasonable, otherwise record an exact uncleared resume condition.
After text native acceptance, select p029 for the BeUI frontend; the existing
user authorization covers that subsequent finite queue.

Outcome: all native paging/fragmentation/lifecycle/halt gates passed.
[Results](../ws019/phase007/results.md).
Next: p029, shared BeUI frontend.

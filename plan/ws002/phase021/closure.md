# p021 closure by user acceptance

Date: 2026-09-09
Status: cleared / completed by explicit user decision

The user directs that the historical invalid-free report remain in the bug
ledger, be treated as likely already corrected, and no longer block p021 or
WS002. This supersedes the requirement to establish the old allocation/free
provenance before closure. No new reproduction or proved root cause is claimed.

Current acceptance remains supported by [q136](results.md): missing-login
PCAT/PC98 boots obey the six-respawn limit, normal login/logout/respawn works,
and exact lifecycle ownership checks pass. [q142](q142-results.md) establishes
the pre-child bootstrap retirement and 400 PCAT / 100 PC98 exact comparisons.
The UHCI repair in p024 is complete, but its identity with the old PC98 failure
is unproven. Historical reports remain unchanged below this current decision.

[BUG-012](../../known-bugs.md) retains the old symptom and uncertainty. Reopen
a bounded corrective phase only if it recurs, retaining architecture/image,
allocator call-site provenance and the first invalid owner; do not infer a TTY
or scheduler defect from the old diagnostic alone. There is no active mandatory
investigation remaining under p021 following this user acceptance.

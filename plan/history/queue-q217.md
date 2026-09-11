# Queue q217: removable UAS transport recovery

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 60 active minutes
Previous: [q216](queue-q216.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](../ws025/phase029/phase.md) | uncleared | Revoke uncertain removable generation, retire old references, checked reset/init once before fresh publication; host failure checks and build |

Never revalidate removable media by capacity equality or replay failed writes.

Result: removable generation retirement and checked transport recovery implemented;
actual-source failure tests, HS/SS native read-timeout/new-publication recovery and
three builds pass. Earlier login/oracle failures retained in results. Full p029
remains uncleared pending final requirements/evidence audit, not physical hardware.

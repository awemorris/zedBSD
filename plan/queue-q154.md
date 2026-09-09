# Queue q154: process-path mounts and source inspection

Date: 2026-09-09
Status: finished
Authorization: User resumed autonomous Priority work after switching to Astra.
Timebox: 150 active minutes
Previous: [q153](queue-q153.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p021](ws019-installation/phase021-nested-mount/phase.md) | completed | Host/sanitizer/races, three builds and native lifecycle pass |
| 2 | [ws019-p004](ws019-installation/phase004-zedinst-existing-fat-overlay/phase.md) | uncleared | Actual native source capture passes; public integration remains |

Standing autonomous authorization covers implementation and bounded tests.
Keep full p004 incomplete until public admission and end-to-end acceptance.

Previous evidence: [q152 results](ws019-installation/phase004-zedinst-existing-fat-overlay/q152-results.md).
Full mount/discovery/confirmation and installed-boot acceptance remain.

Previous result: [q153 evidence](ws019-installation/phase004-zedinst-existing-fat-overlay/q153-results.md).
Preserve bootstrap callers; no top-level-workspace workaround. Existing standing
authorization covers the standard mount implementation and bounded tests.

Result: [p021 acceptance](ws019-installation/phase021-nested-mount/results.md)
and [p004 source acceptance/resume](ws019-installation/phase004-zedinst-existing-fat-overlay/q154-results.md).
New populated-tmpfs teardown defect is planned as p022; it is not hidden by
the path fixture's explicit owned-entry cleanup.

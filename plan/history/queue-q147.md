# Queue q147: terminal identity and resumed GUI acceptance

Date: 2026-09-09
Status: finished
Authorization: User resumed autonomous Priority work after switching to Astra.
Timebox: 60 active minutes
Previous: [q146](queue-q146.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws006-p011](../ws006/phase011/phase.md) | completed | libc terminal identity; native error/bounds tests and three builds pass |
| 2 | [ws006-p009](../ws006/phase009/phase.md) | completed | Actual Xzed PTY/input and restored TTY pass on both topologies |
| 3 | [ws006-p010](../ws006/phase010/phase.md) | completed | Ordinary paired USB-root read, HID and hotplug replay passes |

Dependencies: q142 heap closure, q145 supported builds, q126 evdev/BeUI/source
gates. Extend the maintained USB/HID fixture, with no production input changes
unless a concrete regression appears. Preserve capability discovery and stale
descriptor hotplug checks. Screenshots alone do not prove keyboard delivery.

Evidence: `ws006/temp/q146b/xhci/guest.log`, `xzed-before.ppm` and
`controller-result.txt`. First `q146` omitted opt-in Xzed packages; that fixture
configuration was corrected and the failed run retained. The second failure
is actual libc behavior: isatty uses only a console ioctl and ttyname is fixed
to `/dev/console`. Those failures preceded the q147 correction.

Final evidence: [q147 results](../ws006/phase011/results.md).
The q147b campaign passes every cell without heap instrumentation. WS006 is
complete with the retained q126 consumer gates and user physical confirmation.

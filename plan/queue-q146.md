# Queue q146: WS006 paired USB and Xzed closure

Date: 2026-09-09
Status: finished
Authorization: User resumed autonomous Priority work after switching to Astra.
Timebox: 60 active minutes
Previous: [q145](queue-q145.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws006-p010](ws006-input/phase010-legacy-usb-root-recovery/phase.md) | uncleared | xHCI pre-GUI checks pass; paired replay not reached after GUI failure |
| 2 | [ws006-p009](ws006-input/phase009-consumer-legacy-removal/phase.md) | uncleared | Xzed/zterm starts and executes the typed tty command; libc rejects the PTY. Route correction to new p011 |

Dependencies: q142 heap closure, q145 supported builds, q126 evdev/BeUI/source
gates. Extend the maintained USB/HID fixture, with no production input changes
unless a concrete regression appears. Preserve capability discovery and stale
descriptor hotplug checks. Screenshots alone do not prove keyboard delivery.

Evidence: `ws006-input/temp/q146b/xhci/guest.log`, `xzed-before.ppm` and
`controller-result.txt`. First `q146` omitted opt-in Xzed packages; that fixture
configuration was corrected and the failed run retained. The second failure
is actual libc behavior: isatty uses only a console ioctl and ttyname is fixed
to `/dev/console`. No claimed GUI or paired completion.

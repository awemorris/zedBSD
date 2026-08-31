# Queue: USB HID evdev producers and hotplug

Last updated: 2026-09-01

QID: `q048`

Queue status: finished

Queue finished: **Yes**

Authorization: the user directed HID completion, a `WIP` commit and push, and
then automatic continuation to USB LAN. Report Protocol, stale-fd event-number
reservation, and USB 1.1 support are already resolved. q047 completed the
legacy-HCD concurrency and checked endpoint/device recovery prerequisites.

Timebox: none. The automatic source, lifecycle, regression, build, xHCI, and
paired EHCI/UHCI gates were completed. The single physical keyboard/mouse
observation remains an explicit final acceptance handoff and was not claimed
in this Queue.

Parent: [master plan](master.md)

Previous Queue: [q047](queue-q047.md)

## Purpose

Implement the first production USB HID keyboard, relative mouse, and absolute
tablet producers on the general USB function framework. Publish generation-safe
dynamic `/dev/input/eventN` nodes, feed keyboard reports to the existing input
subscriber/console path, and prove hotplug on xHCI as well as USB 1.1 HCDs
without weakening the general USB lifecycle.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws006-p008` | [Phase](ws006-input/phase008-usb-hid-evdev/phase.md) | completed | Automatic/software milestone: checked Report-Protocol HID keyboard/mouse/tablet, dynamic cdev/devfs generations, stale-fd isolation, console coexistence, safe recovery/hotplug, focused/regression/build gates, and xHCI plus paired EHCI/UHCI QEMU acceptance pass; IN-T42 physical observation remains |

## Fixed boundaries

- Match HID interfaces and descriptors, never a VID:PID or fixed port.
- Report Protocol is mandatory. Malformed or unsupported descriptors fail the
  attach transaction and never fall back to Boot Protocol.
- Unpublish a detached pathname immediately, but keep its immutable old cdev
  and input generation plus `eventN` reservation through the final stale-fd
  close. A new generation cannot alias an old fd.
- Preserve the q047 URB ownership, cancellation/drain, clear-halt, reset,
  UHCI/EHCI concurrency, and USB Storage contracts.
- Do not add hidraw, output/feature reports, LED control, gamepad/media/touch,
  stable numbering, vendor quirks, or legacy-console UAPI removal.
- Use Phase-local tests and disposable QEMU media. Do not run aggregate
  `make check`, consume `.internal/`, or change Noct source.

## Completion definition

q048 is finished at the automatic/software milestone. Production-source
dynamic cdev/devfs generation tests pass ordinary, ASan/UBSan, and analyzer
modes. The USB HID fixture passes 92 checks plus sanitizer, xHCI-lifecycle,
and analyzer gates; IN-T30--IN-T35 ownership regressions also pass. Legacy-HCD,
USB-recovery, and xHCI concurrent-URB regressions pass, as do default PC-98,
amd64, and i386 full builds plus disk-image generation. The i386 build retains
the pre-existing undefined `NOCT_NM` skip in part of its undefined-symbol scan;
the build passes, and a replacement host `nm -u` scan of `build/pcat/vmunix`,
all top-level `build/pcat/*.ELF`, and every `build/pcat/bin` file finds zero
undefined-symbol lines.

`build/q048-p008-xhci-usbonly3` passes capability-only keyboard/relative/
absolute discovery and records, USB-only console control, stale-fd
`event3 -> event4 -> event3` isolation/reuse, and a concurrent 64 MiB read from
the xHCI USB Storage root. `build/q048-p008-paired-uhci-baseline1` passes the
same semantics with HID on companion UHCI and Storage on paired EHCI, including
tablet replacement after the concurrent read. Temporary UHCI diagnostics were
removed and no speculative controller change was retained after the earlier
one-off fault did not reproduce. That fault remains open as WS004 `BUG-008`;
recurrence reopens this Queue's legacy acceptance rather than being treated as
an HID success. Physical IN-T42 was not run and remains the only p008 hardware
acceptance handoff.

# WS008 shared test cases

Parent: [WS008](../ws.md)

| Case ID | Area | Required observation |
| --- | --- | --- |
| NOCT-T00 | Upstream audit | Canonical target/runtime/build/BeUI paths and authoritative revision are recorded |
| NOCT-T10 | Compiler target | Upstream target tests emit and run zedBSD binaries with the documented ABI |
| NOCT-T20 | Runtime | File, memory, process, time, and threading subset tests pass on zedBSD |
| NOCT-T30 | BeUI graphics | `/dev/graphics` drawing/surface/lifecycle tests pass alongside the existing SDL backend |
| NOCT-T31 | BeUI input | evdev keyboard/mouse tests pass without legacy console-event interfaces |
| NOCT-T40 | Package | A clean pinned clone-and-build installs declared files and reports network/cache failures clearly |

This catalog becomes executable after the authoritative Noct source is
available; until then WS008 remains blocked before Phase extraction.


# WS006 shared test cases

Parent: [WS006](../ws.md)

| Case ID | Area | Required observation |
| --- | --- | --- |
| IN-T00 | UAPI | `input_event`, constants, ioctl layouts, timestamps, and 32/64-bit ABI match the published compatibility profile |
| IN-T10 | Input core | Multiple readers, queue overflow/resync, nonblocking read, poll, grab/permissions, and detach wakeup pass |
| IN-T20 | Console bridge | Console text input and evdev readers coexist with modifiers, repeat, and virtual-terminal behavior |
| IN-T30 | Consumer migration | Xzed and BeUI keyboard/relative/absolute pointer cases pass without console event ioctls |
| IN-T40 | USB HID parser | Valid and malformed report descriptors, bit bounds, boot/report protocols, and unknown usages pass |
| IN-T41 | QEMU USB HID | xHCI keyboard, mouse/tablet, hotplug, disconnect, and event delivery pass |
| IN-T42 | Physical USB HID | Target laptop keyboard/mouse devices identify and operate through evdev across reconnect |
| IN-T50 | Legacy removal | No in-tree consumer uses console continuous-event/key-state UAPI and console regressions pass after deletion |

Executable paths are added when each Phase is extracted.


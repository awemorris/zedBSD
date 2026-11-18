# Kei/zedBSD 1.0.0 Beta 2: known issues

Status: reference; the problems known in the 1.0.0 Beta 2 release.

<!-- Draft of 2026-10-09 (ws129-p005, P1), for review. Taken from the open bug tickets and the beta 2 triage
     ("write as known issues"). The "review:" comments name rows that depend on fixes or tests still running; settle
     each one at the release candidate (10/13) and delete the comments.      2026-10-15: Gen9 sleep now keeps the iGPU in D0 (WS237 p005, P2 d3ca76dd0, user option B); checked only by host tests, the SV7 check is pending. -->

These are the problems known when Beta 2 was released, with what you can do
about them. The numbers are the project's bug numbers. Read also the
[security notes](zedbsd-1.0.0-beta2-guide.md#security-notes) in the user
guide.

## Computers

| Problem | What to do | Bug |
| --- | --- | --- |
| Beta 2 is tested on the Dell Latitude 5330 only. Other computers, the Latitude 5320 among them, may start but are not tested. | Use a Latitude 5330. | — |
| On battery, drawing slows down to a few frames a second, and the computer stops without a warning when the battery runs out. | Keep the computer on AC power. | BUG-159 |
| **Shut Down** may leave the computer powered on. | When the screen stays on after Shut Down, hold the power button until the computer turns off. | BUG-119 |
| With an HDMI display connected, the HDMI display stays dark while the lid is closed. | Keep the lid open. To use only the external display, turn the built-in one off in Settings → Display. | BUG-255 |
| The Gen9 graphics driver (6th to 10th generation Core, on by default) does not drive the display yet: the desktop is shown on the firmware's framebuffer (each frame is copied there, so it may tear), the screen stays at the resolution the firmware chose, its brightness and external displays cannot be changed, and Vulkan Video is not offered. While it drives the GPU, sleep keeps the GPU powered (the panel and its light are turned off), and the GPU does not enter its power-saving state (RC6), so the battery runs down sooner. | Keep the computer on AC power, or turn the driver off with `i915.gen9=off` (the desktop does not start without the driver; see [Older Intel graphics](zedbsd-1.0.0-beta2-guide.md#older-intel-graphics)). | — (the display driver is planned for Beta 3) |
| SATA drives work without NCQ (many requests at once are slower), TRIM (an SSD is not told which blocks are free) and hot plugging, and optical drives (ATAPI) are not seen. Link power saving (ALPM, DevSleep) is left off on purpose, because many drives and controllers misbehave with it, so a SATA drive uses a little more power. An NVMe SSD that Intel RST hides behind a controller in RAID mode is not seen. | Connect a SATA drive before you start the computer. For an NVMe SSD behind RST, set the firmware's storage mode to AHCI. | — |
| While a program writes a large amount of data to the disk, the first save that waits for the disk (`fsync`) can take about 20 seconds. The saves after it are quick. | Wait for the save to finish. | — |

## Touch screen, keyboard and mouse

| Problem | What to do | Bug |
| --- | --- | --- |
| Some USB mice with a Logi Bolt receiver do not work. | Use another USB mouse or the touchpad. | BUG-105 |
| A pen that is already near the screen or tablet when the desktop starts, when the tablet is connected, or when you switch back to the desktop does nothing. | Lift the pen away and bring it back. | — |

## Network

| Problem | What to do | Bug |
| --- | --- | --- |
| Some 5 GHz Wi-Fi networks connect but give no address, and the connection drops. | Use the 2.4 GHz network of the same router. | BUG-145 |
| Copying files over a USB Ethernet adapter is slow (about 1 MB/s). | — | BUG-222 |
| In a virtual machine (QEMU), an SSH session to Kei may stop for one or two minutes and then go on, and a command sent over SSH soon after the start may not run. The cause is not known yet. | Wait, or connect again. | BUG-281 |

## Desktop and applications

| Problem | What to do | Bug |
| --- | --- | --- |
| With about 30 windows open, new windows cannot be drawn. | Close windows you do not use. | BUG-120 |
| In the shell, a long Japanese line in the history can break the display and hide the prompt. | Press Ctrl+C for a new prompt. | BUG-173 |
| In `vi`, the cursor can stand off by a column in a line with combining characters or characters of ambiguous width (such as some symbols), because `vi` and the Terminal count their widths differently. ASCII and ordinary Japanese text are not affected. | — | — |
| Bold text looks different from regular text (its edges are smoothed differently). | — | BUG-205 |
| Opening and closing App Home is less smooth than on Linux: the animation runs at a lower frame rate. | — | BUG-280 |
| The Browser does not show WebP or AVIF images, or SVG files used as images (`<img>`); it shows PNG, JPEG and GIF images and SVG drawn inside the page. A GIF animation shows its first picture only, and SVG inside the page is drawn without its outlines (strokes), clipping, masks, filters and text. | — | — |
| Phone calls through a paired phone (Bluetooth HFP) are not supported in Beta 2. The call controls behind **Also use calls** in Settings are untested, and there is no call audio. | Answer and make calls on the phone. Messages and contacts work. | — (planned for Beta 3) |

<!-- review (2026-10-14): the code freeze of 2026-10-14 18:00 took Gen9 (WS229) and AHCI (WS231). The SSH row is a
     suspected problem seen only in the project's own QEMU tests, intermittently. -->

<!-- review (2026-10-14): removed the rows of bugs closed since the draft: BUG-182 (tapping a button in the Browser),
     BUG-170 (the volume slider), BUG-206 and BUG-207 (the Browser's address and loading), BUG-203 (Japanese input in
     Phone), and the commented rows BUG-191, BUG-238 and BUG-242. Bluetooth keyboards and mice passed the 5330 UAT, so
     the Bluetooth row is gone too. The 20-second save was measured in QEMU only (a 500 MB write beside it), and it
     is the same as before the fsync fix. BUG-308 (a crash in the middle of a journal commit may leave a new file
     showing a deleted file's data) is not listed: it needs a crash inside a commit, before fsync returns. -->

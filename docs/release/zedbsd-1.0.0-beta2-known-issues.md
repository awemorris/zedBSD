# Kei/zedBSD 1.0.0 Beta 2: known issues

Status: reference; the problems known in the 1.0.0 Beta 2 release.

<!-- Draft of 2026-10-09 (ws129-p005, P1), for review. Taken from the open bug tickets and the beta 2 triage
     ("write as known issues"). The "review:" comments name rows that depend on fixes or tests still running; settle
     each one at the release candidate (10/13) and delete the comments. -->

These are the problems known when Beta 2 was released, with what you can do
about them. The numbers are the project's bug numbers. Read also the
[security notes](zedbsd-1.0.0-beta2-guide.md#security-notes) in the user
guide.

## Computers

| Problem | What to do | Bug |
| --- | --- | --- |
| Beta 2 is tested on the Dell Latitude 5330 only. Other computers, the Latitude 5320 among them, may start but are not tested. | Use a Latitude 5330. | — |
| The firmware's ACPI table (DSDT) of the Latitude 5330 is not read completely. Battery, lid and power button events can be missing or wrong. | Keep the computer on AC power. | BUG-165 |
| On battery, drawing slows down to a few frames a second, and the computer stops without a warning when the battery runs out. | Keep the computer on AC power. | BUG-159 |
| **Shut Down** may leave the computer powered on. | When the screen stays on after Shut Down, hold the power button until the computer turns off. | BUG-119 |
| Closing the lid may do nothing. With an HDMI display connected, the HDMI display stays dark while the lid is closed. | Keep the lid open. To use only the external display, turn the built-in one off in Settings → Display. | BUG-253, BUG-255 |

<!-- review: BUG-253 has a fix waiting for the 5330 UAT; if the UAT passes, keep only the HDMI half (BUG-255). -->

## Touch screen, keyboard and mouse

| Problem | What to do | Bug |
| --- | --- | --- |
| In the Browser, tapping a button on a web page does not press it. | Use the touchpad or a mouse in the Browser. | BUG-182 |
| Some USB mice with a Logi Bolt receiver do not work. | Use another USB mouse or the touchpad. | BUG-105 |

## Network

| Problem | What to do | Bug |
| --- | --- | --- |
| Some 5 GHz Wi-Fi networks connect but give no address, and the connection drops. | Use the 2.4 GHz network of the same router. | BUG-145 |
| With both a wired adapter and Wi-Fi connected, unplugging the cable can drop Wi-Fi too, and Settings may show Wi-Fi as the active network while the cable is in. | Turn Wi-Fi off and on again in Settings. | BUG-212, BUG-189 |
| Copying files over a USB Ethernet adapter is slow (about 1 MB/s). | — | BUG-222 |

<!-- review: BUG-189 and BUG-212 close if the next 5330 UAT does not reproduce them (2026-10-08 user). -->

## Desktop and applications

| Problem | What to do | Bug |
| --- | --- | --- |
| Dragging the volume slider can freeze the desktop for a while. | Tap a point on the slider instead of dragging it. | BUG-170 |
| With about 30 windows open, new windows cannot be drawn. | Close windows you do not use. | BUG-120 |
| In the shell, a long Japanese line in the history can break the display and hide the prompt. | Press Ctrl+C for a new prompt. | BUG-173 |
| Bold text looks different from regular text (its edges are smoothed differently). | — | BUG-205 |
| The Browser does not respond while a page loads, and editing the address can put `file://` in front of `https://`. | Wait for the page; type the whole address again. | BUG-207, BUG-206 |
| Japanese input does not work in Phone. | — | BUG-203 |
| Printing a PDF to some network printers (for example the Brother MFC-L3770CDW over IPP) fails. | Print over LPD if the printer offers it. | BUG-271 |

<!-- review: the rows below are the bugs the beta 2 triage plans to fix before the RC, and the ones whose fixes wait for
     T1. Move a row into the table above only if it is still open at the RC.
| In Settings → Wi-Fi, clicking the switch to turn Wi-Fi off can press Scan instead. | — | BUG-184 |
| In Settings → Wi-Fi, a single tap on a network does not join it. | Tap it twice. | BUG-188 |
| The Log Out icon ends the session without asking. | Save your work first. | BUG-235 |
| Choosing a running app in App Home starts it again instead of switching to it. | Switch with the top bar's app list or the Windows key. | BUG-232 |
| Maximizing a window by double-clicking its title bar takes almost a second, and a maximized window dragged down grows to full size once before it shrinks. | — | BUG-179, BUG-180 |
| Opening a program in /bin from Files shows nothing. | Run it from Terminal. | BUG-234 |
| The key repeat settings in Settings do not change the repeat. | — | BUG-191 |
| Emacs does not take -nw, and the output of M-x shell is laid out wrongly. | — | BUG-238, BUG-242 |
-->

<!-- review: Bluetooth (WS143) is in the release on the condition that keyboards and mice work on the 5330 by the
     freeze (2026-10-09 user: "in, and a known issue if it does not work"). If they do not, add under "Computers":
| Bluetooth keyboards and mice do not work yet. The Bluetooth page in Settings is there, but pairing does not complete. | Use USB keyboards and mice. | — |
-->

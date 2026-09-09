# Queue q165: public diskpart GPT initialization

Date: 2026-09-09
Status: finished
Authorization: standing autonomous Priority instruction; queue presented before implementation
Timebox: 90 active minutes
Previous: [q164](queue-q164.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p032](ws019-installation/phase032-diskpart-gpt-init/phase.md) | completed | Public init command, complete proposed layout under one reservation, cancellation/fault/native/build gates |

This prerequisite connects the accepted GPT codec and kernel reservation.
p006 still owns formatters and automatic installer layout; p007 owns native installation.
BeUI remains mandatory p029 after text installation.
[q165 results](ws019-installation/phase032-diskpart-gpt-init/results.md): host
ordinary/sanitizer, blank/existing QEMU, independent GPT checks and all three
builds pass. Next: partition reservation and FAT32/native UFS block formatting.
Capture and display the installer framebuffer on the next installer execution.
WS004-p050 remains mandatory in this goal after the full installer is finished.

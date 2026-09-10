# Queue q185: PC98 graphical FAT installation

Date: 2026-09-10
Status: finished
Authorization: user requested PC98 graphical FAT install with two IDE HDDs
Timebox: 180 active minutes
Previous: [q184](queue-q184.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p050](ws019-installation/phase050-pc98-graphic-fat/phase.md) | uncleared | Noct i386 packaging, PC98 FAT/BIOS backend, two-IDE graphical normal-path installation and installed boot |

The amd64 installer is cleared. This is additional platform support. No native
PC98 installation/GPT and no exhaustive abnormal-case acceptance.

Result: packaging and copy work complete; BUG-020 prevents pristine swap
verification and destination-only boot. Resume with focused ownership-safe
cache reclaim regression in q186.

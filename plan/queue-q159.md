# Queue q159: public installer package and acceptance

Date: 2026-09-09
Status: finished
Authorization: User resumed autonomous Priority work after switching to Astra.
Timebox: 120 active minutes
Previous: [q158](queue-q158.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 0 | [ws019-p025](ws019-installation/phase025-public-runtime-contracts/phase.md) | completed | /bin/noct, shell exit status and null/zero; 20 host cases, native 16 checks and maintained builds passed |
| 1 | [ws019-p026](ws019-installation/phase026-noct-image-copy/phase.md) | completed | Noct copy/progress: 73 host fault cases, native cancel/install/rerun; p004 conflict remains |
| 2 | [ws019-p004](ws019-installation/phase004-zedinst-existing-fat-overlay/phase.md) | completed | Public cancel/install/rerun and corrected conflict continuation passed; see q159 results |

Standing autonomous authorization covers [q159 design](ws019-installation/phase004-zedinst-existing-fat-overlay/q159-design.md).
[q159 results](ws019-installation/phase004-zedinst-existing-fat-overlay/q159-results.md).
p004 is complete; installed-boot acceptance remains p005.
WS025-p032 physical observations were received: PC-9821V13, 64 MB, IDE CF,
blank-screen beep immediately after memory test/possible boot-sector entry.
They are recorded in its phase. QEMU-only acceptance suffices for p029 when
transport works. [q156 results/resume](ws019-installation/phase004-zedinst-existing-fat-overlay/q156-results.md).

# Queue q208: held descriptor across physical UAS replacement

Date: 2026-09-10
Status: finished
Authorization: standing autonomous Priority execution
Timebox: 120 active minutes
Previous: [q207](queue-q207.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws025-p029](../ws025/phase029/phase.md) | uncleared | Guest keeps old block fd open across unplug/replacement; old read/write/fsync refuse, close permits new binding, new pattern readback |

Compile a test-only guest helper; inject only disposable boot media. Replacement
backing has a distinct pattern and must remain unmodified by old-fd writes.

Result: held-old-fd physical replacement passes on SS and HS; old read/write/fsync
reject, closing permits new media publication/readback, replacement remains
unchanged and halt passes. Full p029 retains in-place SCSI media exchange and
uncertain-write/error work; repeated pending-detach diagnostic noted.

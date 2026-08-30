# zedBSD known bugs

Last updated: 2026-08-30

This ledger records observed defects that are deliberately outside the active
Queue.  A listed item is not silently treated as a failure of an unrelated
Phase; it returns to implementation only through a later finite Queue.

| ID | Area | Status / priority | Observation | Resume and acceptance condition |
| --- | --- | --- | --- | --- |
| `BUG-001` | legacy PC/AT IDE ATA CACHE FLUSH | Open; deferred, below the primary amd64 UEFI path | A disposable BIOS/IDE overlay boot can intermittently report `ata: sda op=2 ... error=5 status=C0` while FAT is synchronizing the writable overlay. `op=2` is `BIO_FLUSH`; the same frozen image reaches `login:` on retry, and amd64 UEFI/xHCI USB boots are unaffected. The observation first appeared during `ws018-p004` and recurred once during the `ws018-p009` runtime matrix. | When explicitly selected, boot 1,000 pristine disposable copies with the same image, retain every failure log and seed/topology, measure the rate, then fix the ATA flush completion/timeout path. Accept only after the corrected 1,000-run campaign has no unexplained flush error and ordinary amd64 UEFI regression still passes. |

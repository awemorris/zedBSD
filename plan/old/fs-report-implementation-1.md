# FS/USB functional correction plan — q086

Date: 2026-09-06
Baseline: 02eeed5. Inputs: fs-report-1..4 and fs-report-respond-1.md.
Authorization: user explicitly requested planning, 50 scenarios, queue and execution.
Review progress every 90 active minutes, as in q085; this is a checkpoint,
not permission to discard incomplete acceptance. No commits or aggregate make check.

## Decisions

R1 is P0: eliminate the synchronous caller-buffer ownership trap before BOT retry.
A failed cancel must not let a late HCD completion access the caller's returned
stack/BIO buffer. Use preallocated URB-owned staging for synchronous storage;
retain the URB and its staging under the existing HCD reference when retirement
cannot be proven. Reject reuse until ownership drains. Retry cancellation within
a finite budget. Do not publish a fictitious terminal result or free DMA.
Other endpoint requests and subsequent BIO error returns must remain possible.
Checked HCD retirement remains the reclamation boundary. No automatic bus-wide
stop while holding the storage mutex: unrelated driver callbacks may need locks.

R2: BOT recovery precedes the final flush-error latch. A final failed flush stays
visible; subsequent success must not reopen a filesystem made readonly by failed
metadata rollback. Confirmed remount-rw is a separate later design.
R3: expose bounded experimental chunk and IMOD settings, record four cells
512/4096 × 4000/0 (160 optional). Separate operation-count evidence from actual
IRQ/latency evidence; host models cannot establish physical USB timing.
R4: retain ordered logical extents, validate full coverage and disk bounds, use
claimed writes through the parent disk cache. Serialize/invalidate the FAT sector
slot at the mapped-I/O boundary; attach-only invalidation is insufficient.
R5: CSW STALL gets one clear-halt/read-CSW retry. Then a bounded reset/reissue
ladder, and one reset-UA retry only within this operation after our successful
reset and unchanged USB generation. No blind retry for medium-change/not-ready.
Do not infer self-reset merely from sense 06/29. Use one operation deadline.

Zero-fill remains until bytes are initialized before pointer publication.
Full FAT write-chain validation remains; loop removes repeated traversal through
its immutable claimed map. Optimize existing full-block UFS overwrite locally.
Check generated images for the present multi-block directory update limitation.
Do not duplicate the forthcoming single 64-bit UFS design (WS024).

## Execution order

1. ws018-p017: baseline/experimental controls, image-directory audit, measurements.
2. ws004-p049: finite synchronous ownership, BOT recovery before latch.
3. ws018-p018: syscall batching, parent-cache loop map, local FS improvements.
4. ws018-p019: 50 acceptance stories, production fixtures + native QEMU,
   serialized amd64/PCAT/PC98 builds and Wi-Fi regression.

The first measurement phase and USB implementation may interleave when a
baseline hangs, with the exact source and failing gate recorded.
Tests use disposable images and public synthetic credentials only.
No hardware claim will be inferred from a fake HCD or QEMU result.

## Deferred, not claimed complete

Write-back cache; universal file page cache; FAT general validated cursor
generations/claim index; full UFS metadata batching and multi-block directory
mutation belong to subsequent bounded work, coordinated with WS024.
Controller-wide recovery/automatic re-enumeration after failed hardware barriers,
external hubs, large-capacity SCSI, UAS and remount-rw remain separate follow-ups.
Initial q086 fixes the synchronous hang without pretending an unresponsive
endpoint was restored. Record any uncovered defect with a concrete next gate.

Acceptance definitions: [50 scenarios](fs-acceptance-50.md).

## Implementation refinements and evidence

The immutable logical map is borrowed by the FAT file state, rather than bypassing
file_io from loop_submit. Thus the existing inode/VM content transaction and
claimed filesystem-write inheritance still protect the parent cache's wider RMW.
Map unbind does not discard an unrelated dirty FAT sector.

EP0 has an independent HCD-inflight admission marker: a timed-out caller may
return while the retained request continues to exclude subsequent EP0 commands.
This was added after the existing binding-transaction regression exposed the
distinction between caller transaction lifetime and HCD request lifetime.

Storage's wire/reissue budget is 15 seconds, checked between transfers and passed
as remaining time to CBW/data/CSW/control transfers. Checked cancel/drain and the
common clear-halt/HCD endpoint reset have their own bounded recovery grace; this
is not a claim of an exact 15-second wall-clock upper bound on every controller.
Normal zero-timeout public USB calls retain their explicit indefinite-wait API.

R3 includes four actual kernel builds and QEMU xHCI USB-root boots, in addition
to host operation counts. Final defaults are restored after experimental builds.
Physical IRQ/CPU histograms remain unmeasured.

All residual report findings are tracked in [follow-ups](fs-report-followups.md).

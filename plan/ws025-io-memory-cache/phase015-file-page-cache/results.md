# WS025 p015 execution results

2026-09-07; q105 completed. Intermediate checkpoints below are historical.

## Implemented ownership

`vm-object.c` and private `vm-object-cache.inc` remain the single file-content
owner. Ordinary reads prepare optional cache admission before inode/position
locks, then use the existing operation reference and content-read lease. A new
cache object opens a separate read-only backend handle and verifies its final
content inode, so the user's last close still closes their own description.
Existing mapping-owned objects keep their prior lifetime policy; cache-created
objects may subsequently acquire mappings and retain clean data after unmap.
The last mapping releases its write-capable handle after synchronous writeback.

At most 32 optional objects each hold 16 optional pages (2 MiB payload); mandatory
mapping/pin ownership is not counted as disposable cache. A sole ordinary reader
can retire its previous clean window, while concurrent/mapped readers force a
bounded direct fallback over absent pages. Existing resident shared contents
remain authoritative. The fill borrows the existing nonblocking pool and publishes
up to 16 busy pages for one internal 64 KiB backend read. Complete initialized
pages become valid; a short backend result preserves its exact returned prefix
and leaves incomplete pages retryable. Allocation/pool refusal uses the leased
backend without creating unbounded cache storage.

Independent cache teardown exposed wakeup lifetime boundaries: operation and
content/resize final wakeups, and registry-waiter release, now retain the object
until wakeup finishes. Clean eviction requires sole registry ownership, no
mapping/operation/waiter/pin/dirty/writeback error, and closes files outside locks.
Unmount drops eligible cache paths before its existing busy check. Formatter,
loop and swap discard optional readers before taking their ownership locks;
shared backing-claim admission now checks VM aliases after publication, preventing
an independent cache/mapping owner from surviving into loop/swap/format ownership.
Cache reads acquire the block-device lifecycle token even on hits. Full physical
media replacement/recovery identity remains p025, not a new claim here.

## Current evidence

- Maintained formatter/VM fixture passes after the lifetime adapter.
- `temp/p015-budget-resize-2.log`: ordinary and ASan/UBSan pass approximately
  282k checks each; counts vary with actual four-thread allocation races.
  Tests cover reopen with zero backend reads, 16 KiB single-call populate,
  shared/normal write coherence, mapping and pin exclusion, short read 5000 bytes,
  each of four page-metadata allocation failures, pool absence, read error/retry,
  EOF and device refusal, 256 KiB sequential reads as four 64 KiB backend calls,
  40 files against the 32-object cap, and truncate-tail invalidation.
- Earlier `p015-lifetime-2.log` failed because the test reopened a formatter FD
  without its required O_NOFOLLOW. The descriptor was corrected; the production
  reservation rule was retained. `p015-budget-resize.log` expected two resident
  pages after shrink, but the existing resize contract revokes the partial tail
  too; the test now expects the preserved first page and validates repopulated
  contents/EOF. Both rejected attempts remain in temp.
- amd64 and PC/AT builds passed; PC-98 is running. The compiler dependency file
  includes the private cache implementation.
- `plan/ws018-kernel-architecture/temp/ws025-p015-native-first` passes the existing
  grouped native storage run and reboot verification on disposable xHCI USB.

Remaining: native warm-read/copy-up/MAP_SHARED fixture, final full storage and
selected regression gates, remaining build results, source hashes and P/W/M closeout.
Physical gate is user-accepted; no agent-executed physical test is claimed.

## Final boundary refinements

`p015-rw-races.log` exposed the inherited host wait stub's intentional CHECK(0)
on real waiting, not a production failure. The WS025 fixture now supplies an
atomic sequence wait/wake model with a five-second deadlock diagnostic. The
maintained original fixture remains unchanged. Four read/retirement workers,
one writer and a mapping/final-put worker now run concurrently.

`p015-dirty-first.log` found a production defect in the new retention path:
successful retry via inode sync left RETAINED_WRITEBACK set on a clean cached
object. The final implementation clears a resolved error classification under
object lock and releases the no-longer-needed writer outside locks. A selected
short fill ending before an unaligned requested byte returns EIO instead of
falsely reporting EOF. `p015-ownership-final.log` passes all ownership, failure,
budget/window and EOF cells plus the maintained formatter cells, ordinary and
ASan/UBSan (350,370 / 343,520 checks; concurrent counts depend on scheduling).

The initial dedicated native cache fixture (`temp/p015-cache-native`) passed
last-close/reopen with UFS content reads=0 and USB reads=0, independent FD writes,
real MAP_SHARED stores/msync/unmap, truncate/EOF, pinned lower versus copied-up
upper identity, and unlink/recreate. The complete FS50/Wi-Fi30/native matrix passed
in `plan/ws018-kernel-architecture/temp/ws025-p015-final`.
`temp/p015-baseline` passed 202 aligned 16 GiB samples and its counter/readback
oracle, both modes 10/20/20 ms p50/p95/p99. These precede only the final failed-
writeback classification and pre-offset short-read fixes above; no latency gain
is claimed. Final x86 rebuilds and selected native revalidation are in progress.

## Completion

The final production revision passes amd64/PC-AT/PC-98 builds
(`p015-amd64-final-2.log`, `p015-pcat-final.log`, `p015-pc98-final.log`).
`temp/p015-cache-native-final/results.json` repeats the dedicated native cache
oracles successfully with zero warm UFS content and USB reads.
`plan/ws018-kernel-architecture/temp/ws025-p015-final-2/results.json` passes
S01–S50, Wi-Fi 30 in both variants and native grouped reboot persistence.
`temp/p015-final-source.json` records production/fixture identities;
`git diff --check` passes. No production change follows these gates.

CACHE01–CACHE04 and the EOF/failure/unmount/lifecycle portion of CACHE05 are
covered by the host/native cells above. Physical replacement identity and
quarantine recovery remain explicitly owned by p025; managed-RAM budget growth
and clean-only pressure are p016. This phase retains synchronous write-through
and the existing mapped-object error ownership; it does not enable delayed I/O.

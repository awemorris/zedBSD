# WS024 p003 ongoing implementation

2026-09-07, q101, completed; final WS acceptance and retirement follow in p004.

Target `ufs-format.[ch]`, the maintained C image backend and the single Python
`ufs_format.py` now emit the frozen 256-byte inode / 64-bit pointer geometry.
The independent unified checker also handles inline symlinks. Group count
expands to bound each bitmap to one 8 KiB block. Target construction retains
streaming bounded buffers and initializes all reserved/allocated metadata on
prefilled input; free data remains unspecified under the existing contract.

The explicit journal-snapshot profile reserves disjoint tail regions, initializes
ZUJ1/ZSL1 locators and an inactive ZSN1 control, and excludes them from filesystem
size. Target callbacks select a per-call context under the unchanged formatter
reservation transaction; no mutable global profile state. Ordinary remains the
default. Target CLI is `mkfs -t ufs [--profile=journal-snapshot] FILE`.

`temp/p003-images-first` records 12 target write/production-decoder/readback cells
(ordinary + ASan/UBSan, 4/32/192 MiB, both profiles), six three-producer comparisons,
and two sparse near-2-GiB maximum-size target checks. All pass. C backend and target
images are byte-identical; Python differs only in its independently generated
filesystem ID, normalized solely in the comparison fixture. A 192 MiB image
uses four groups, establishing that the old fixed-two-group limit is removed.

Normal manifests, target package recipe, Noct architecture/data builders and
Python platform/root/check consumers now select the unified paths. The first
coherent amd64 rebuild passes in `temp/p003-build-first.log` (exit 0), including
unified target mkfs and regenerated architecture/data/packed disk images.

`temp/p003-formatter-fault.log` passes 12,521 checks in each ordinary/sanitizer
variant: unsupported sizes before I/O, short/error/interrupted reads and writes,
withheld primary publication, corruption detection and prefilled-media metadata
replacement with free bytes preserved. Maintained Noct data images match exactly.

Still pending: completed build result, maintained fault/reservation/CLI refusal
fixtures, optional profile discovery through the mounted production driver,
extattr/quota/namespace persistence gates, active fixture/dependency migration,
all supported builds and native runtime. Superseded production source files are
retained until p004 retirement. No runtime or phase completion is claimed here.

Unified ordinary QEMU xHCI 16 GiB aligned baseline passes 202 samples with
readback, no completion errors and source-image hash preservation. Both modes
remain p50/p95/p99 = 10/20/20 ms. Evidence: WS025 `temp/ws024-p003-baseline`;
current sample oracle uses appended unified counters, historical samples retain
their original event prefix. No latency improvement is claimed by consolidation.

`temp/p003-features-recovery` passes the complete mounted-feature/reboot gate:
51 run checks and 15 reboot verification checks. The initial attempt's boot
regression and retained UFS1 reopen policy are documented in p002 results.
`temp/p003-format-file.log` passes 2,650 checks per variant through the actual
CLI and descriptor transaction, including both retired type refusals and
explicit feature-profile admission. Supported 32-bit builds are underway.

Both configured PC/AT and PC-98 `make -j16` builds pass (exit 0):
`temp/p003-pcat-build.log`, `temp/p003-pc98-build.log`. The normal amd64 image
is being restored before the target CLI/native-to-overlay runtime gate.

## q101 completion

WS019 `temp/ws024-p003-combined/result.json` is PASS combined: actual target
mkfs, old type/invalid target refusal, native UFS startup, switch to newly
formatted overlay upper and a further reboot/readback, with partition tables,
FAT boot sectors, sentinel and production source hashes preserved. Explicit
Noct feature-profile generation also passes the unified checker. Earlier
"pending" paragraphs record execution checkpoints; the final remaining work
is p004's full feature matrix and old-source/active-fixture retirement audit.

# p031 execution results

Status: completed / cleared q188 by user acceptance (2026-09-10). Intermediate progress and failed attempts below are retained as history.

- Corrected the planning generator's graphics path typo to the user's agreed platform/<machine>/graphics tree before moving sources.
- Saved preimplementation source hashes, configuration and source copies in ../temp/p031-baseline.
- Independent userland UFS codec: formatter ordinary/sanitizer each 12,521 assertions pass; maintained image generator byte equality passes.
- Layout and 372-symbol naming stages each passed the amd64 kernel build before style transformation.
- Current style stage uses the existing repository declaration/control/layout tools, with subsequent compiler and runtime checks. Conditional diagnostic declaration movement and switch declaration issues were detected and corrected.
- DMA vector ordinary/sanitizer pass. UFS journal ordinary/sanitizer each 39,193 checks pass, including snapshot flush/error and journal image ownership gates.
- Source-linked host fixtures now extract disposable translation units from marked sections of the current consolidated production source. Header dependency metadata and hashes are recorded; full-kernel builds and native runtime remain required integration gates.

Remaining: finish coding-style review and exceptional ordering records, supported platform builds, all affected host fixtures, FS50/Wi-Fi30/native USB/NVMe, mkfs host independence and fresh-rootfs/standalone packaging checks, final symbol/tree audit and ordinary artifact restoration.

No physical runtime for this patch has been claimed. No commit or aggregate make check.

## Intermediate verification

- All 24 integration host runner families pass in p031-host-1, before final declaration-order cleanup.
- amd64, PC/AT and PC-98 kernel builds pass after style fixes. Full image builds are in progress.
- Exact source tree matches all 106 target files. The three x86 builds expose 993 driver definitions and no unprefixed external definition in the inspected current objects.
- Device suite: 36/39 initially passed. NVMe lifecycle needed the relocated test-header include path. RTL table provenance now compares the complete ordered numeric stream and counts against hash-pinned upstream sources, allowing formatting changes. Both rechecks pass.
- AX211 runtime/scan had a stale expected TRUNCATED result for a 23-byte iteration record: the pre-style scan implementation reproduces the same assertion failure. The decoder already classifies misaligned result storage as OVERSIZED. Corrected that expectation; ordinary, real API89, sanitizer, analyzer and 32/64-bit syntax gates now pass. Production scan behavior was preserved.
- Thus all 39 selected device runners have passing results, including the three documented rechecks.

The latest coding-style file was already user-modified at entry (modeline/goto prose removed, checklist still references goto). Existing ownership cleanup gotos were preserved rather than mechanically changing lifetimes to satisfy the inconsistent checklist. Ordering exceptions are listed in style-order.tsv. Callback table initializers require their function declarations before the table.

## q188 update (2026-09-10, in progress)

The user clarified that RTL8822B's separate `.inc` is intentional for license
separation. The attempted inline merge was reverted byte for byte against
`../temp/q188-layout1/rtl8822b-before.c` and
`../temp/q188-layout1/rtl8822b-tables-before.inc`. Production driver/table and
provenance checker have no q188 difference. Phase and source map now preserve
this exception. The layout.json with 110 files describes only the abandoned
intermediate merge, not the restored final tree (111 source files).

Retained useful changes: core/provenance runners read current production paths
without the obsolete section-marker extractor; independent mkfs host compilation
now includes block-command.c, fat32-format.c and the public block ABI header.
`../temp/q188-userland2/results.json`: isolated kernel-free build, cflow/cxref
host operation and standalone install, all three rootfs placements PASS. The
first attempt's missing mkfs command symbols were a stale test source list.

Three image builds passed (`/tmp/zedbsd-q188-{amd64,pcat,pc98}-build.log`)
before reverting the table merge. Current actual object lists (not stale .o
files on disk) exposed 531/364/109 external driver definitions, all drv_, in
57/36/20 driver objects; `../temp/q188-layout1/*-symbols.json`. Non-x86
conditional source coverage is not yet proved.

RTL core ordinary/ASan+UBSan/analyzer passed on the intermediate merged source;
LSan disabled for this run. Provenance test passed on that intermediate source
with a temporary slicing change, since removed. Do not claim these as a fresh
post-restoration run. Earlier production behavior was restored without changes.
Wi-Fi stories 1–36 passed ordinary and sanitized in
`/tmp/zedbsd-q188-wifi-stories2.log` and
`/tmp/zedbsd-q188-wifi-sanitize.log`; initial unprivileged run failed because
local socket creation was denied. Authorized unrestricted run passed.

Remaining: migrate remaining marker-dependent host fixtures (especially FS50
runner and its merged kernel collaborators), source/conditional style-symbol
review, reconcile later runtime evidence and verify any uncovered gates. The
old FS50 runner still references removed io-stats.c and section fragments and
expects exactly 30 Wi-Fi stories although 36 now exist. p031 is not cleared.

### q188 FS acceptance migration

Current-code FS50 passed: `../../ws018/temp/q188-fs-combined/results.json`.
39 host scenarios (`q188-fs5`) plus 11 native USB scenarios (`q188-native1`)
cover all S01–S50. Host source.sha256 and native source-image SHA were checked
against the worktree/image before combining the independent records. Native
run and verify booted the disposable USB image twice. Its producer was rebuilt
with the current source using storage-native.mk; ordinary disk-image was also
rebuilt after restoring the separate RTL tables. No installer fault campaign.

Changes are fixture-only: full FAT/UFS/io translation units replace obsolete
section fragments; UFS uses its actual journal rather than duplicate abort
stubs, with the existing deterministic disk-fault medium and host IRQ shim;
removed single-block FAT API assertions are superseded by the adjacent extent
checks for contiguous, empty and fragmented files. The syscall fixture still
executes the exact extracted production functions and current I/O counters;
only its existing two host pool boundary functions override the merged io.o.

Both ordinary and ASan/UBSan storage groups passed. Host LSan was disabled
where ptrace prevents it; Wi-Fi ordinary and sanitizer runners passed all 36
stories in the authorized socket-capable environment. The old assertion
requiring exactly 30 stories now requires all original 30 and permits the
six later recovery cases. The 39/50 standalone host result is retained as
such, not relabeled; the combined result supplies the native missing cases.

AX211 still has the original two source-section markers, unlike the reordered
UFS/xHCI/input units. Its runners now select only that valid production unit
when generating disposable test views; extraction checks/hashes are retained.
Do not generalize the marker removal to every driver. RTL USB runner directly
includes the current RTL core with its separate licensed table file.

### q188 device-fixture progress

- AX211 ten formerly global-extractor-dependent runners all PASS in
  `../../ws004/temp/q188-ax211/results.json`: boot, command, core,
  DMA, firmware loader, PCI lifecycle, runtime-start, scan-session, transport
  backend and transport. Each runs its maintained ordinary/sanitizer/analyzer
  and ABI checks; production AX211 source unchanged.
- RTL8822BU driver fixture PASS ordinary/ASan+UBSan/analyzer with the restored
  separate license file: `/tmp/zedbsd-q188-rtl-driver.log`.
- Dynamic cdev/devfs fixture PASS ordinary/ASan+UBSan/analyzer, including full
  current input.c analysis: `/tmp/zedbsd-q188-input2.log`. Its new block-admin
  dependencies are aborting host stubs, not success stubs; these character-
  device scenarios never execute that unrelated path. Actual block-admin
  behavior remains covered by WS019 acceptance.

No production function was made extern or otherwise changed for these tests.
The remaining legacy xHCI/DMA/FS auxiliary runners must still be reviewed
for stale extraction and source-shape checks, and final platform/style
coverage reconciled. q188 and p031 remain in progress, not cleared.

## Final disposition

User accepted completion after manual refactor/device review and repaired-test
smoke acceptance. p031 is cleared; q188 is finished. Earlier remaining-gate
paragraphs describe interim state and are superseded by this decision.

Historical xHCI/DMA/auxiliary source-shape and extraction-based runners not
migrated in q188 remain maintenance candidates. Non-x86 and exhaustive style
rechecks were not performed in this cycle; neither is claimed PASS. These
items no longer block subsequent WS025 phases under the user's acceptance.
No new production changes, commits, or aggregate make check were made.

旧テスト群の包括的整理は、ユーザー指示で概要のみ作成した
[WS026](../../ws026/ws.md)へ引き継ぐ。p031のclearは維持する。

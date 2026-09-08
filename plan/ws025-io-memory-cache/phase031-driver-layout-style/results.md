# p031 execution results

Status: in-progress, q123. Implementation and acceptance are not complete.

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

# WS025 p013 execution results

2026-09-07; q104 completed. Earlier checkpoints below retain their chronology.

## Implementation

Explicit synchronous FAT entry transactions hold up to eight old/new 512-byte
sector images and 32 entry replacements. Each mirror retains its own previous
contents, including packed FAT12 neighbors and FAT32 reserved bits. Prior data
initialization/directory detachment is flushed before publication; all new copies
are flushed before return. Failed write/flush restores every old copy and flushes;
uncertain restoration freezes the filesystem and invalidates cached metadata.

The allocation-free fallback also captures each mirror's own entry value in a
bounded 256-element stack array, with the same durability and rollback boundaries.
Admission is explicit; a backend error after admission is never mistaken for a
workspace refusal and silently retried as fallback.

Free chains commit bounded entry groups while retaining the old chain for restoring
previously completed groups after a later error. Growth creates a private zeroed
chain within one FAT sector and the bounded transaction/initialization limits,
then publishes it with a same-sector old tail. A cross-sector tail is linked only
after the new chain commits. A new directory's dot entries are flushed before its
parent name is published. No table mutation remains deferred after the operation.

## Evidence so far

- Baseline `temp/p013-cost-before.log`: 16 KiB create/append followed by 32 KiB
  unlink. FAT12 writes 318/321/257; FAT16 192/193/129; FAT32 193/193/130.
  An initial 32+32 KiB test exceeded the deliberately small FAT12 fixture; that
  rejected attempt is retained as `p013-cost-too-large.log`.
- Intermediate single-cluster transaction `p013-cost-after.log` reduced writes,
  but required 65 sync calls for create/append. This prompted bounded private-chain
  growth within the phase, rather than accepting that avoidable wait overhead.
- `p013-cost-runs.log`: before the final empty-file slot adjustment, create/append
  use 70/69 writes and 5 syncs on FAT12/16; FAT32 adds one creation metadata write.
  Unlink uses 5/5/6 writes and 4 syncs. Final cost/fault variants are in progress.
- `p013-fat-vfs-final.log`: maintained native FAT12/16/32 VFS, mirrors and 1024-byte
  logical-sector tests pass ordinary and ASan/UBSan (441,113 checks each).
- `p013-private-host-2.log`: 1,652 direct transaction checks pass, including packed
  sector crossing, read/write/flush failures, write-then-error, failed rollback,
  preserved differing old mirrors in fallback, workspace and sector-budget refusal.

Test oracles were updated at their semantic boundaries: mount-sync measures its
own added flush after ordered allocation work; the truncate recovery-allocation
fault targets that allocation's size; growth measures its actual write count and
injects an error at every write instead of assuming the previous six writes.
Earlier failures and intermediate results remain in temp logs.

Remaining: final cost/fault variants, cursor/cache/loop regressions, supported
builds, native storage and selected performance verification, final P/W/M updates.

## Directory and final focused gates

The same sector-image commit engine now handles directory runs. FAT32 long-name
creation merges same-sector entries and flushes preceding LFN sectors before the
final SFN sector. Deletion merges its bounded LFN/SFN run as well. Replaced the
old repeated exact-slot write/restore helpers; neighboring and hidden end-marker
bytes are captured in the whole-sector old image. Rollback failure still freezes
the filesystem, even when a deterministic test medium happened to retain old bytes.

`temp/p013-directory-4.log` passes the maintained VFS matrix in ordinary and
ASan/UBSan (439,579 checks each). Fixed-count fault oracles now require a consumed
failure for every measured actual write and stop at the first unconsumed ordinal;
all namespace/contents/free-space/remount/error oracles remain. End-marker tests
target the single grouped publication and its rollback write.

`temp/p013-directory-batch-2` passes 2,054 direct fault/ownership checks and 233
public cost/contents checks per ordinary/sanitizer variant. The direct fixture
verifies that two-sector LFN publication sees the preceding sector in the durable
medium, all active logical epochs end, and unchanged neighboring bytes survive.
Final 16 KiB create/append and 32 KiB unlink writes are 68/69/5 for FAT12/16 and
68/69/5 for FAT32; sync calls are 3/5/6 for FAT12/16 and 5/5/6 for FAT32. These
replace the high-overhead intermediate single-cluster results above.

`temp/p013-cache.log`: sequential cache reuse, seek validation, other-open writes,
truncate/free/growth pass 1,692 checks per variant. `p013-cursor.log` and
`p013-cursor-sanitize.log`: fragmented boundaries, sparse handoffs and every actual
in-loop write fault/retry pass 97,939 checks each. `p013-write-cost.log`: distant
chain corruption/read errors before mutation and traversal bounds pass 1,660,787
checks. amd64 build passed; PC/AT build is in progress, with PC-98, final normal
amd64 restoration and disposable native acceptance still pending.

## Final acceptance

All three supported x86 builds passed (`p013-amd64-build.log`,
`p013-pcat-build.log`, `p013-pc98-build.log`); normal amd64 was restored
(`p013-amd64-restore.log`). The full storage runner passed S01–S50,
Wi-Fi 30 scenarios in ordinary/sanitizer variants, and both native USB guest
boots: `plan/ws018/temp/ws025-p013-final/results.json`.

`temp/p013-baseline` passed all 202 samples and readback/protected-source
oracles with 16 GiB RAM and aligned disposable backing. Both modes report
10/20/20 ms p50/p95/p99. No latency improvement is inferred from this clock
resolution. Final source hashes are in `temp/p013-final-source.json`;
`git diff --check` passed. Physical acceptance is user-accepted, not an
agent-executed hardware test.

Production ownership remains `fat.c` plus its private `fat-batch.inc`;
there is no deferred FAT metadata owner. p015 proceeds with file-cache
lifetime, independently of this synchronous operation batching.

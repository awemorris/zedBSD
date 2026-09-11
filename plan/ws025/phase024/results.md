# p024 execution checkpoint

Status: completed in q120 (2026-09-08 JST).

Implemented owned DMA vectors, transactional shared USB/HCD staging reservations,
xHCI bounded SG TD construction/short accounting, and a synchronous disk vector
adapter. The HCD reservation owns staging; USB core borrows it. Caller data is
still isolated by a copy and may be released after a failed bounded cancellation
without freeing the outstanding hardware backing. Arbitrary direct user DMA is
not enabled. Disk vector compatibility batching uses the existing bounded pool;
its fallback reports split work explicitly and preserves confirmed BIO prefixes.

Focused ordinary and ASan/UBSan evidence:

- `../temp/p024-dma-vector-1.log`: actual DMA vector and coherent allocator with
  controlled HAL frames; high/low mask, page boundary, fragmentation, fallback,
  allocation failure, busy free/repin and device owner accounting.
- `../temp/p024-sg-register-1.log`: actual USB core, xHCI reserve/SG and storage
  reserve fixtures. Includes shared HCD registration validation, failed growth,
  late completion after caller free, short event offsets, 16-page TD, ZLP and
  malformed segment rejection.
- `../temp/p024-disk-vector-3.log`: 162 checks per variant, actual disk/BIO/error
  ledger with controlled backend; one 64 KiB gather/scatter, full prevalidation,
  claim refusal, hardware splitting, pool exhaustion and confirmed error prefix.

Native evidence so far:

- `../temp/p024-build-amd64-2.log`: supported amd64 build passes.
- `../temp/p024-native-normal-2/`: ordinary 8 GiB, four CPU, UEFI/xHCI USB boot;
  writeback, fsync, age worker, unmount/remount and sequential/random readahead
  pass. Final readahead jobs/running/demand/errors are zero.
- `../temp/p024-native-sg-1/`: link-only forced discontiguous 16-page staging,
  actual hardware emulation with all the preceding combined tests passes.
  Existing PCI DMA policy is 32 bit: all four 64 KiB owners use low addresses
  within that mask. This is not a native high-address DMA claim; high RAM/vmap
  and 64 bit DMA vector checks have their separately identified evidence.
- `../temp/p024-native-sg-2/`: timing-enabled repeat, completed PASS. One 64 KiB overwrite + fsync window measured
  usb_copy=264432, hcd_copy=0, shared=264432, sg_td=5, trbs=168,
  trb_bytes=264432, elapsed=60 ms, TSC delta=141181568. The window includes
  metadata traffic; TSC is elapsed guest cycles, not process CPU consumption.

A real integration failure was caught: the first native run rejected the new
HCD capability in core registration and could not mount USB root. The capability
mask and callback/dependency checks were fixed, a registration regression was
added, and the second ordinary native run passed. Preserve
`../temp/p024-native-normal-1/` as failed diagnostics.

Remaining: identical-work copy baseline and timing comparison; broader lifecycle
and async/cache regressions, supported pcat/pc98 builds, forced ordinary amd64
relink (probe removal alone does not relink), wrapper absence check and ordinary
native rerun, final source hashes and M/W/P/Q synchronization. Do not leave the
probe image as the final ordinary artifact.

## Controlled copy comparison

`../temp/p024-native-copy-baseline-1/` completed the same combined native suite.
The link-only baseline disables shared staging at HCD registration, preserving
identical 16-page backing and SG TD construction. For the same 64 KiB overwrite
plus fsync (including metadata), both paths recorded USB copy 264432 bytes,
5 multi-segment TDs, 168 data TRBs and 264432 TRB bytes. Additional HCD copy was
264432 bytes in the baseline versus zero in shared staging. Total observed copies
therefore fell from 528864 to 264432 bytes. Both single samples were 60 ms; TSC
deltas were 143392450 versus 141181568. This does not establish a latency or CPU
utilization improvement. It establishes removal of the duplicated copy without
changing the physical SG shape or hiding metadata traffic.

## Final focused regressions and CPU measurement

`p024-usb-recovery-1.log` passed the production core recovery (1111 checks per
ordinary/sanitizer variant), binding (971 per variant), function model (1833),
HCD unregister, analyzer and actual x86 HCD/core/storage object gates.
`p024-final-async-bio-1` passed 1196 checks per variant; DMA/pool/cache workers,
readahead worker (252 per variant), and writeback policy all passed ordinary and
ASan/UBSan in the corresponding `p024-final-*-1` directories.

Both `p024-native-copy-cost-1` and `p024-native-sg-cost-1` completed the native
combined suite. Copy and TRB counters reproduced exactly. The full writeback
command (including typing, age sleep and many fsync operations) used QEMU host
CPU 3.05 s / wall 6.063 s for the copy baseline, versus CPU 3.51 s / wall 6.563 s
for shared staging. This coarse single comparison is noisier and actually higher
for shared staging; no CPU speedup is established or claimed. The isolated
64 KiB overwrite still measured 60 ms in both, with elapsed TSC 157843979 versus
150562553. Copy reduction and correctness, not an inferred throughput improvement,
are the demonstrated result. Raw command-level measurements are retained in
`command-costs.json`, with an explicit host-emulation/typing measurement boundary.

Production and fixture source hashes are in `../temp/p024-final-source-1.json`
(814 files). Supported builds and ordinary artifact restoration are currently
running; the phase remains in progress until those and ordinary native acceptance
finish.

## Completion

The final file-cache host gate, supported pcat/pc98/amd64 builds passed in
`p024-final-{file-cache,pcat,pc98,amd64}-2.log`. An explicit space.c relink restored
the ordinary kernel and `p024-final-wrapper-check.txt` confirms no SG/vmap probe
symbols. `p024-final-native-normal-1/` then passed the full 8 GiB four-CPU USB
writeback/journal/readahead/remount suite. All 814 frozen source hashes still
match. No builds or runtimes were left running at phase closure.

SG01–SG05 are supported by the owner/TD/mask/fault fixtures, real discontiguous
low-mask USB transfers, high-address owner fixture and p023 high vmap evidence,
copy/TRB comparison and explicitly bounded CPU/timing measurements above.
ASYNC01–08 regressions pass. Existing MEM12–16 evidence remains, with no DMA
capability promotion: PCI retains its conservative DMA32 policy. Physical
acceptance remains user-accepted, not agent-measured. The synchronous disk
adapter does not claim native scatter support in every backend or remove the
necessary isolated client copy. Performance benefit established here is reduced
copy bytes; latency and CPU improvement remain unproven.

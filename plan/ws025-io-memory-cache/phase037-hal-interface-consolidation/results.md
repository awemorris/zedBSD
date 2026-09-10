# HAL consolidation results

## q247: physical allocation arguments

Removed hal_pmem_request from 46 active production/test files. Both allocator APIs
and internal helpers now take paddr, size, alignment, type and attr explicitly;
range allocation additionally retains minimum, maximum, boundary and descriptor.
All architecture implementations and reusable fixtures updated. Existing allocation
validation, descriptor ownership and constrained fallback semantics preserved.
Source manifest: `../temp/q247/source.json`; file list: `changed.json` alongside it.
Active source/test scan has no request structure. All allocation declarations and
calls checked for six/nine arguments respectively. No wrapper structure retained.

Focused actual VM/file fixture passed ordinary and ASan/UBSan/leaks (`../temp/q247-vm2`).
Actual vmap/scratch owner fixture passed 18060 checks in each ordinary/sanitizer
variant (`../temp/q247-vmap`, `../temp/q247/vmap.log`). This is a regression gate
for the existing owner before its later replacement, not endorsement of its HAL
placement. Constrained allocator compatibility test passed rejection and rollback
ownership with src/hal/pmem-constraints.c.
Initial VM fixture compilation exposed unused expanded stub parameters and an
old macro-renamed forwarding call; these test adaptations were corrected before
passing. The old failed log is retained in q247-vm1.

Sequential supported disk-image builds all exited 0:
- `/tmp/zedbsd-q247-amd64.log`
- `/tmp/zedbsd-q247-pcat.log`
- `/tmp/zedbsd-q247-pc98.log`

No new runtime or non-x86 acceptance claimed. Next implement approved SYS mapping,
query/range API and common VM ownership, migrate scratch/DMA/uaccess and retire
hal_vmap/hal_kernel_page_lookup. p037 stays uncleared until these complete.

## q248: system page-operation and query boundary

hal_space_query now accepts an optional physical-address result in all five HALs;
existing VM callers explicitly pass NULL. On successful absent-page queries PA is
zero and PRESENT is clear; user queries preserve page-aligned input semantics.
The new kernel range API reports the amd64 dynamic window when RAM mapping is
ready; i386/ARM64/m68k/SPARC report empty until their dynamic SYS mapping exists.

amd64 map/prot/prot_query/unmap/clear_flags accept HAL_SPACE_SYS within the dynamic
window only. A single system_space table list/serializer is shared with the
existing vmap implementation, avoiding parallel lock domains during migration.
System operation lifetime is immortal; shootdown normalizes its internal owner to
HAL_SPACE_SYS to include all CPUs. Supervisor flags, rollback and table retirement
reuse existing page operations. Upper-half query also walks fixed mappings and
resolves byte offsets through 4 KiB, 2 MiB and 1 GiB leaves.

Actual extracted page-operation fixture passes ordinary and ASan/UBSan/leaks:
`../temp/q248-system2/` (source/extracted functions/logs). It verifies user/system
range exclusion, supervisor/NX, overlap rejection, protection and A/D, partial map
rollback, absent query and physical translation for all three leaf sizes. Table
allocation and shootdown are controlled collaborators; no real SMP latency claim.
Initial fixture compile failed due to duplicate PAGE_SIZE/static declaration;
fixture declarations corrected before passing. Actual VM/file suite and vmap/
scratch suite pass ordinary/sanitizers (`../temp/q248-vm`, `../temp/q248-vmap`).

Three disk-image builds exited 0: `/tmp/zedbsd-q248-amd64.log`,
`/tmp/zedbsd-q248-pcat.log`, `/tmp/zedbsd-q248-pc98.log`.
No native SYS runtime acceptance yet. Other architecture PA queries updated but
non-x86 runtime not tested. Existing hal_vmap and hal_kernel_page_lookup still
exist: next migrate common VA/frame/lifetime ownership and all three consumers,
remove those APIs and run native scratch/DMA/user-view regressions. p037 uncleared.

## q249: common kernel mapping owner and HAL removal

Moved VA slots, owned/borrowed physical vectors and pin/retirement state into
common vm.c (vm_kernel_map_*; header kern/vm-kernel-map.h). Metadata uses a leaf
lock never held across HAL/allocator calls. All mappings use hal_space_map/unmap
with HAL_SPACE_SYS. Failed population retires only installed translations before
freeing owned frames; borrowed frames are never freed. Slot/maximum-size policy
remains bounded as before; it is now independent of the amd64 HAL.

Scratch, DMA and uaccess migrated. DMA physical segments use hal_space_query and
require PRESENT. Removed hal_vmap_* and hal_kernel_page_lookup from hal.h; deleted
amd64 space-vmap.inc. Active source/tests scan contains no old API/file references.
The common owner still calls map per page; contiguous physical runs can be batched
through the existing range API without adding another HAL interface.

Focused tests passed ordinary/sanitizers:
- Common owner allocation/map failures at all 16 pages, retry, borrowed readonly/
  writable ownership, pins, slot exhaustion and real scratch integration: q249-map3.
- DMA vectors, segment grouping, mask fallback, prepare/retire failures and accounting:
  q249-dma1. Runner repaired to compile actual consolidated DMA/cache/io sources.
- uaccess view orchestration, absent optional common symbol fallback: q249-view.
- Actual VM/file regression: q249-vm.

The old vmap host fixture was replaced with an extraction of the actual common
owner in its own translation unit. Two initial test compilation attempts failed
because the optional symbol check shared a translation unit with its definition;
separate units preserve production weak-symbol semantics. No production checks
were disabled to pass this test.

Default amd64/PCAT/PC98 builds exited 0 in /tmp/zedbsd-q249-{amd64,pcat,pc98}.log.
Experimental output-enabled amd64 also built and native q249-output-on passed
READ/PREAD, EOF untouched suffix, unaligned fallback and fork/COW/exit. Eight rounds
confirmed 128 MiB direct output and zero copyout; CPU 5.27 s, wall 5.25 s. This is
slower than the earlier pre-migration q244 cell (2.59/2.60 s), not a controlled
current-off comparison or physical performance claim. Keep views default off;
coalescing map runs is a next optimization. Source/guest/image hashes retained
in q249-output-on; source image unchanged. Ordinary amd64 rebuilt afterward,
/tmp/zedbsd-q249-amd64-default.log exit 0; no live build/QEMU remains.

p037 remains uncleared for native owned-frame/scratch/DMA and input-view migration
acceptance. Next use the migrated native owner probe, then optimized range mapping
and direct-I/O comparison. API/owner removal is complete; full acceptance is not.

## q250: native common-owner acceptance

Migrated link-only owner probe passes on disposable UEFI USB-root QEMU with
8 GiB and four CPUs (`../temp/q250-owner/result.json`, guest.log). Both real
subordinate table allocation failures restore physical free bytes. Three rounds
of 16 high, deliberately nonadjacent pages remain visible through existing and
new user spaces and all APs; each release restores free bytes to 8574963712.
Forced contiguous refusal routes 16 large pool slots and 69632-byte scratch
allocations through the common VM owner. Root login succeeds. Source image hash
is unchanged and QEMU is terminal. Reusable run-kernel-map-qemu.py retains argv,
source hashes and results, terminates only its own process, and checks login.

Probe build and forced ordinary rebuild both exited 0, logs
`/tmp/zedbsd-q250-probe-build.log` and `/tmp/zedbsd-q250-default-build.log`.
The ordinary kernel has no __wrap_ symbols (restoration.txt). Probe image retained
separately under q250-probe-image. No production or HAL API changes in this queue.

Owned-frame/scratch/SMP native acceptance is now covered. Native DMA and input
view migration acceptance, plus map-run performance work, remain; p037 stays
uncleared. Pending trap/syscall consolidation awaits the explicit stack-contract
decision and was not implemented under this queue.

## q251: input-view native acceptance and current baseline

Current off/on input cells both PASS (`../temp/q251-input-{off,on}/result.json`).
Identical guest SHA256 04101d7b69e15c50f1890c5bdbd7b060e1651e940f5b3a8eb519c98132056509.
WRITE/PWRITE data/readback, post-view CPU stores, unaligned fallback, append,
37-byte RLIMIT_FSIZE short write and fsync pass after common VM migration.
Eight 16 MiB samples attribute 134217728 bytes exclusively to copy (off) or
view (on), respectively. Source image unchanged in both cells; both QEMU
processes terminal. Current production hashes retained in source.json.

Off CPU/wall totals: 2.89 / 2.86 seconds. On: 5.98 / 5.99 seconds. CPU ratio
2.0692; this single TCG pair is not physical performance evidence, but provides
no basis to enable the path by default. Per-page common-owner map calls remain
an optimization candidate; causation has not been isolated by measurement.
Experimental and restored ordinary builds exit 0 in /tmp/zedbsd-q251-input-build.log
and /tmp/zedbsd-q251-default-build.log. No wrapper symbols in restored kernel;
experimental image retained separately at q251-input-image/hdd.img.

Native input acceptance is covered; p037 remains uncleared for native DMA.
p028 remains uncleared/default off for performance and its broader acceptance.
Next optimize contiguous map runs using the existing HAL API, then compare again
and complete migrated native DMA acceptance. No new HAL APIs in this queue.

## q252: contiguous physical map runs

Common kernel-map population now acquires/captures the full vector before mapping,
then calls existing hal_space_map once per physically consecutive run. A 64 KiB
contiguous vector uses one HAL call instead of sixteen. Repeated/nonadjacent frames
remain distinct runs. Only successful runs count toward rollback; all mappings
are retired before owned frames are freed. No HAL declaration changes.

Actual extracted common-owner test passes ordinary and ASan/UBSan/leaks in
`../temp/q252-map2`. Existing all-16-page allocation/map failure cases remain,
with added owned/borrowed contiguous call counts, exact PA translation and repeated
run boundaries, second-run failure, absent translations after rollback and retry.

First PCAT build rejected a UINT64_MAX comparison against 32-bit hal_physaddr_t;
comparison now uses that type's own maximum. Final focused tests pass. Final
PCAT, PC98 and amd64 disk-image builds exit 0, logs /tmp/zedbsd-q252-pcat2.log,
/tmp/zedbsd-q252-pc98.log and /tmp/zedbsd-q252-amd64-final.log. Ordinary defaults
retained; no runtime/build process remains. Initial q252-map also passed on host
but q252-map2 records the final portable source.

No native performance claim yet for the new grouping. Next run the current native
input comparison and forced fragmented DMA acceptance. p037 remains uncleared;
p028 still defaults off and must satisfy its broader requirements.

## q253: coalesced input native pair

Current off/on cells both pass with identical guest SHA256
04101d7b69e15c50f1890c5bdbd7b060e1651e940f5b3a8eb519c98132056509.
Evidence: ../temp/q253-{off,on}/result.json, guest.log and source.json.
Each variant transfers 128 MiB in eight timed rounds with exact copy/view
attribution. WRITE/PWRITE, readback, append, CPU store, unaligned and short-limit
checks pass. Both source images unchanged; QEMU terminal.

CPU/wall totals: copy 2.87/2.87 seconds, input view 4.68/4.68 seconds. Earlier
q251 view was 5.98/5.99 seconds. Grouping reduces observed view CPU about 21.7%,
but current view still costs about 63.1% more than copy. This is a single TCG pair,
not physical performance acceptance. Keep direct views default off; remaining
alias lease/page-table costs need attribution after native DMA acceptance.

Experimental and restored ordinary builds exit 0 in /tmp/zedbsd-q253-build.log
and /tmp/zedbsd-q253-default.log. Ordinary kernel contains no wrapper symbols.
Experimental image retained under q253-input-image. No production changes in
this queue. p037 retains native DMA acceptance; p028 retains performance work.

## q254: fragmented native DMA (32-bit effective mask)

Disposable 8 GiB / four-CPU UEFI USB-root + USB journal-snapshot filesystem passes
writeback batching/age, fsync, synchronous paths, unmount and two remount readback
checks. End stats: workers/busy/dirty/reserved/tickets/memory/errors all zero.
Evidence ../temp/q254-dma/{guest.log,results.json,dma-evidence.json,source.sha256}.
Four actual DMA vectors report 64 KiB / sixteen discontiguous segments.
QEMU is terminal; source image hash unchanged. Probe and forced ordinary builds
exit 0 in /tmp/zedbsd-q254-build.log and /tmp/zedbsd-q254-default.log.
Ordinary kernel has no wrapper symbols; probe image retained at q254-probe-image.

The intended >4 GiB DMA gate did NOT run: all vectors report bits=32 and addresses
0x20000000..0x203f0000. PCAT PCI root constraints in pci-pcat.c fix address_bits=32;
xHCI inherits this DMA device, even though QEMU hcc1=0x00087001 advertises AC64.
No production policy was relaxed just to pass. This is a bus/device capability
policy limitation, not a failure of the tested low fragmented transfers.

Native DMA migration works under current effective constraints. p037 retains the
explicit high-DMA gate from this queue. Resume by designing bus versus device DMA
address constraints and xHCI AC64 handling (preserving 32-bit device masks), or
by a bounded test-only high-capability DMA device that exercises real xHCI after
checking hardware capability. Do not claim high DMA from q250 high CPU mappings.
The broader p028 performance and p030 IRQ work remain independent.

## q263: explicit high-capability experiment failed its probe

Added optional WS025_SG_HIGH_QEMU link-only HCD registration fixture. On the
known QEMU AC64 xHCI, it creates a 64-bit DMA device with existing API before
HCD start; owner remains alive for HCD guest lifetime. Production bus mask
unchanged. Probe build exits 0. New log confirms hcc1=0x00087001 and two
64-KiB/16-segment bits=64 vectors at 0x110000000..0x1101f0000.

Campaign then stops at sg-kernel.c count == 16 assertion before login/data
acceptance. No native high-DMA PASS. Could be contiguous fallback after forced
PFN allocation refusal; actual cause is not measured yet. Evidence
../temp/q263-high-dma/{guest.log,failure.json}, /tmp/zedbsd-q263-high.log.
Runner exits 1 and cleans up QEMU; attempted targeted termination found that
process already gone. No live build/QEMU remains. Forced ordinary rebuild exits
0 (/tmp/zedbsd-q263-default.log), nm confirms no wrapper symbols. Probe image
retained at q263-probe-image.

Resume: instrument only fixture allocation returns plus actual segment count/
addresses to distinguish injected exact-PFN collisions from common-owner faults.
Do not weaken the high/fragmented oracle or alter production masks just to pass.
p037 stays uncleared; pending HAL trap API discussion unaffected.

## q264: high-DMA acceptance and phase completion

Diagnostic run demonstrated HAL_ERR_NOMEM for exact test PFN 0x110200000,
followed by a valid one-segment contiguous fallback; the probe then rejected
count != 16. This was not evidence of invalid production DMA. Test allocator
now tries at most 1024 nonadjacent candidates on NOMEM, keeps all high/address
constraints and reports skipped candidates. Guest fatal detection now aborts
the private writeback runner promptly so its finally cleanup executes.

Final ../temp/q264-high-final PASS: AC64 hcc1=0x00087001, four real 64-KiB DMA
vectors, each 16 nonadjacent >4 GiB segments, writes/fsync/unmount and two
remount/readback checks succeed. Skipped candidate addresses are retained in
guest.log; acceptance.json records vectors/source identity. Source image
unchanged; runtime terminal. Initial diagnostic failure retained separately at
q264-diagnostic. No production allocator/mapping/mask changes were needed.

Both probe builds and forced ordinary restoration exit 0; logs
/tmp/zedbsd-q264-build.log, -build2.log, -default.log. Ordinary vmunix has no
wrapper symbols; experimental image at q264-probe-image. All jobs terminal.

### p037 completion audit

| Requirement | Evidence |
| --- | --- |
| Expanded pmem arguments, all HALs/callers | q247 source migration and three builds; no request struct remains |
| SYS page operations / PA query / range | q248 actual operation tests; q250 native mappings/SMP |
| Common owned/borrowed lifetime, rollback | q249 owner/VM/uaccess/DMA tests; q252 final run-group tests |
| Scratch/DMA/uaccess consumer migration | q250 native scratch; q249 output, q251/q253 input; q254 DMA32 and q264 high DMA |
| Old HAL mapping APIs removed | Current src/include search has no hal_vmap, kernel lookup, request struct or old map names |
| Supported builds | q252 final amd64/PCAT/PC98 after last common production change; q264 ordinary amd64 restoration |

p037 completed. High-DMA test explicitly supplies an AC64-capable HCD owner;
production PCAT bus still conservatively limits DMA to 32 bits. Other HALs report
empty dynamic kernel ranges as designed and retain fallback; this does not claim
non-x86 dynamic SYS implementation or native runtime. p028 performance/default
adoption and pending syscall/fault interface discussion remain separate work.

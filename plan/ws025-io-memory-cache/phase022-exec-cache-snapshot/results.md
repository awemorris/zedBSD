# ws025-p022 results

Status: completed in q118 (2026-09-08 JST). q117 evidence below records the earlier input-only checkpoint.

## q117 immutable input lease

Added file_exec_snapshot_begin and connected all four ELF main/interpreter entry
points. Direct regular inputs require both read-only mount and read-only disk,
matching visible/content inode identity and no backing/format claim. Eligible
leases hold a content read gate, file/mount lifetime, physical disk cache token,
and optional canonical cache pin. They hold no inode I/O mutex across miss reads;
overlapping immutable leases coexist. Unsupported/mutable/stacked inputs retain
file_content_lease_begin's existing exclusive revoke/writeback/copy protocol.

Cached reads use the real coherent object reader. Optional object allocation
refusal keeps the same content gate and uses FILE_IO_VM_OBJECT internal pread,
which cannot wait on its own shared gate. No positional state changes. EOF and
negative/oversized lengths retain existing checks; bad inode size is rejected
before optional cache preparation and rechecked under the acquired gate.
Both forms unwind every file/cache/device/gate owner; repeated end is harmless.

Evidence:

- `temp/p022-input-host-5.log`: actual file/VM/cache tests, ordinary 424157 and
  sanitizer 428006 checks, plus formatter reservation 626 each. New cases verify
  same cache object, warm reads without backend requests, overlapping read gates,
  unchanged content generation, disk tokens, detached-device error, optional
  allocation refusal with progress, EOF/negative offset, invalid size cleanup,
  and fallback for mutable mount or merely read-only mount on writable disk.
- Earlier host-1/2 logs retain diagnostics: setting cache target zero still allows
  an empty object, so object refusal now uses the real allocation fault hook;
  early bad-size rejection avoids constructing a cache before invalid input fails.
  These do not relax production ownership checks.
- `temp/p022-input-pool.log` and p022-input-pool/: production pool and verbatim ELF
  copy fallback pass both variants. It is still anonymous-copy loading at this stage.
- `temp/p022-input-{pcat,pc98,amd64}-final.log`: supported make -j16 builds pass.
- `temp/p022-input-native-final.log` and
  `plan/ws018-kernel-architecture/temp/ws025-p022-input-final/`: real USB QEMU
  boots/execs, native storage scenarios and reboot persistence pass. Earlier native
  result before the added full-lease disk token is retained separately.
- `temp/p022-input-source.json`: selected exact source hashes. All handles terminal;
  ordinary amd64 restored; git diff --check passes. No commit/aggregate make check.

q118 next adds owning pinned snapshot pages and private VM mappings/COW. A cached
copy is not evidence of physical text sharing, and these intermediate tests do not
complete EXEC01–EXEC05/CACHE01–CACHE09. Physical gate is user-accepted, not measured.


## q118 private cache pages, VM COW and ELF integration

Snapshot owners eagerly capture/pin full canonical file pages and charge their
pointer array/metadata to CACHE_MEMORY_FILE_META. They retain an immutable input
lease; no independent shadow data cache or normal mapping reference is introduced.
Reverse mappings disappear before the last owner releases page/input/device pins.
Private VM regions retain owners through fork, split, unmap and failed publication.
Initial PTEs stay read-only; write fault and writable uaccess create ordinary private
backing after commit admission. Failed PTE replacement can refault old source bytes.

The ELF loader shares only nonwritable, full page-aligned file interiors. Partial
edges, BSS and writable segments remain anonymous. Optional admission failure uses
copy fallback before altering the mapping. Mutable/stacked files preserve the
exclusive copy snapshot; normal writable binaries do not gain blanket ETXTBSY.

Final evidence:

- `temp/p022-elf-share-6.log` and directory: actual file/cache/vmspace/private
  reclaim/ELF production objects, ordinary + ASan/UBSan, 9637 checks each. Two ELF
  loads share identical text PFNs; second-load shared-text backend reads are zero.
  This host boundary intentionally has one pool slot: the copy path borrows it,
  so three edge/data reads bypass cache on the warm load. This is recorded rather
  than reported as zero total I/O; the real pool separately passes its own tests.
- Same fixture: main and ET_DYN interpreter, nonaligned leading/trailing bytes,
  zero BSS, writable data isolation, mutable backing changed after load, fork
  after private changes, untouched shared pages, mprotect commit/PTE failures,
  first writable uaccess without a read fault, failed COW PTE replacement/retry,
  split/partial unmap, 32 successive allocation fault sites, and final zero
  commit/file/object/page/content-gate/device owners.
- `temp/p022-final-file-cache-1.log`: ordinary 425368 / sanitizer 425978 checks,
  formatter reservation 626 each. Actual cache lifetime/concurrent coherence,
  pressure/resize/pinned retention, delayed-write/error and snapshot partial
  acquisition unwind regressions pass. This also retains CACHE01–CACHE09 owners
  exercised by p015/p016/p018; p022 does not replace their policy acceptance.
- `temp/p022-share-pool.log`: real pool and extracted production copy fallback,
  ordinary + sanitizer pass. `p022-share-exec-preparation-2.log`: real exec.c
  preparation passes; the first invocation used a retired pre-refactor filename
  and never compiled. Supplying the current source via the runner's argument
  resolved that fixture invocation error without changing production behavior.
- `temp/p022-share-{pcat,pc98,amd64}.log`: supported make -j16 builds pass.
- `temp/p022-share-native.log` and WS018 `temp/ws025-p022-share-final/`: native
  amd64 USB grouped tests and reboot persistence pass. Native boot exercises the
  ordinary executable path; immutable physical sharing is measured by the actual
  production ELF/VM host fixture, not inferred from QEMU boot success.
- `temp/p022-share-source.json`: production identity. git diff --check passes;
  all executions terminal, ordinary amd64 restored, no commit/aggregate make check.

EXEC01–EXEC04 map to the real ELF/VM tests above and maintained content-lease
concurrency tests. EXEC05 combines snapshot pin/reclaim/refusal tests with the
existing content mutation gates and retained file/mount/device owners. Eligibility
requires a physically read-only disk as well as a read-only mount; writable disks
and overlay/copy-up identities do not enter sharing. There is no new remount-to-RW
operation in this phase. Existing disk/loop retirement admission remains authoritative.
Physical-machine acceptance is user-accepted, not newly measured by the agent.

Earlier p022-vm-share-2 exposed writable uaccess bypassing cached COW. Both its
ready shortcut and final multi-page pin revalidation now reject writable COW pins;
the subsequent actual VM/ELF fixtures verify isolation. Intermediate ELF test
failures recorded the one-slot pool behavior and a host disk with missing geometry;
the fixture now reports text-specific reads and supplies valid disk geometry.

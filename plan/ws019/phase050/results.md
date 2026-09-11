# PC98 graphical FAT installation — completed / cleared q186

The existing amd64 installer was cleared in q184 under the user-approved
normal-path scope. This is additional PC98 support, not a reopened amd64 gate.

- Canonical Noct 2.0.1 builds for i386 with the existing zedBSD adapter and
  interpreter (486 baseline). External acquired sources remain manifest-verified;
  the i386 toolchain and ABI link selection are owned by the zedBSD package.
- PC98 CI image includes /bin/noct and both /sbin installer entry points.
- Real QEMU PC98 486/64 MiB/CoreGraph displays the supplied UI at 640x480 RGB24.
  `temp/q185-probe2/graphic.png` was inspected and shown to the user.
- Existing uname -a reported only i386. The PC98 command now includes pc98 in
  its version field while uname -m remains i386; Noct detects that exact token.
- PC98 wizard offers FAT coexistence and shell only. Uses the mounted root-image
  device to exclude the source disk, FAT16 BPB geometry including 1024-byte
  logical sectors, <=4 GiB partitions and <=2 GiB regular payloads.
- PC98 files: BOOTZBSD.EXE, vmunix, rootfs.img, data.img, swapfile,
  BOOTZBSD.CFG. data/swap are freshly formatted through existing commands,
  avoiding copying mutable source root data or an active swap backing file.
  The same managed-file transaction, Noct copy loop, hashes and UI progress
  are reused. Existing PC98 bootstrap sectors are retained.
- Builds q185-pc98-2 through -4 passed. The first build selection did not include
  packages because package availability names platforms, not architectures;
  this was corrected to amd64/pc98 before any runtime claim.
- q185-install1 could not boot: a cloned target retained the source FAT UUID,
  so boot0 UUID resolution correctly rejected ambiguity. Fixture now gives the
  destination a distinct FAT serial. No kernel workaround was introduced.
- q185-install2 is currently running against build -4: two IDE HDDs; actual
  uname platform detection, source/mode/disk/FAT selection reached preparation.

Fixture scope: existing PC98 FAT16 with compatible bootstrap sectors, but no OS
payload files. No GPT or native provisioning. This does not establish generic
DOS bootstrap replacement or physical PC-9821V13 boot acceptance.

## Normal-path blocker investigation

q185-install2 (64 MiB) and install3 (128 MiB) reached the graphical FAT
selection, then failed while SHA-256 reading source/rootfs.img, before YES
and before creating target files. Both report cksum Input/output error.
Separate q185-read1/read2/read3 probes passed the full file hash without GUI,
with one disk, two disks, and both source/target FAT mounts respectively.
q185-read5 additionally passed the same Noct instRequire/Process/timeout calls
for loader, kernel and rootfs, without GUI. read4 had an incorrect PC98 '='
key mapping and did not execute the intended command; no claim from that run.

Code inspection found PC98 IDE lacks the controller mutex present in PC/AT
IDE. Added one shared mutex across both multiplexed banks, spanning selection,
PIO and error sampling; release before BIO completion. This fixes a concrete
unserialized register interface but is not yet proven to cause this failure.
Build -5 passed; install4 repeats the original 64 MiB normal-path case.

install4 timed out at the disk screen without a kernel error: the fixture sent
Enter before the PC98 redraw/input arming completed. The test now waits for
three equal framebuffer captures before sending input. install5 then passed
the formerly failing source hash and reached the actual YES review, after the
IDE mutex fix. It failed at mkdir of a FAT16-incompatible long staging name.
The PC98 staging directory is now the 8.3-compatible ZINSTTMP.

install7 reached YES then correctly rejected an unrepresentable FAT directory
mode (0700); use the default FAT mode inside the root-only tmpfs workspace.
install8 then copied approximately 32 MiB before File.read failed and new
Process.spawn calls could not start. No successful install is claimed.
Reduced image residency from six to two/three active screen assets. install9
was deliberately interrupted on disposable copies after identifying a further
copy allocation issue: Noct File.read owns an external malloc buffer, while GC
charges only the Packed header for that representation. File.readExact uses
inline GC-managed payloads. Since image/copy lengths are known, those reads now
use File.readExact; no new API or weakened content verification. The general
upstream external-Packed accounting issue is not claimed fixed.

The i386 build now consistently uses the project's soft-float ABI. Its earlier
CMake cache was moved to temp and the target rebuilt from a fresh build directory
(-8). The source tree identity remains unchanged. uname platform detection now
checks the final pc98/i386 fields, not a hostname that happens to be pc98.

install10 (64 MiB, build -11) passed source preparation, kernel/rootfs copy and
verification, and fresh data initialization using managed buffers. It then hit
the common 180-second timeout while truncate allocated the 64 MiB FAT swap file
(status 124). No memory/spawn failure recurred. PC98 now supplies a finite
900-second budget for data/swap preparation commands; unrelated command limits
and the actual source/target sizes and verification stay unchanged.


## q185 build -12 and isolated shared-cache failure

install11 passed swap creation with the PC98 preparation budget, then failed
`mkswap --verify-pristine` with status 1 / ENOMEM. This is not a timeout.
The destination-only boot has therefore **not** passed and p050 is not cleared.

The isolated q185-memory1 probe reproduces this on a precreated pristine
64 MiB swap file, with two IDE disks, 64 MiB RAM and the graphical progress
screen active. It does not need a complete installation first. The verifier
uses 4096-byte pread calls, not a whole-file allocation.

| Counter | Before | After failure |
| --- | ---: | ---: |
| Physical free bytes | 17596416 | 8302592 |
| Shared cache target | 16777216 | 16777216 |
| Shared cache resident | 7483392 | 16777216 |
| Shared cache pending | 0 | 0 |
| Shared cache refusals | 0 | 2284 |
| BIO component capacity / physical failures | 0 / 0 | 0 / 0 |

Reproducer: `python3 plan/ws019/tests/run-pc98-cache-memory-qemu.py
plan/ws019/temp/NEW-UNUSED-DIRECTORY`. It prints STATUS and BEFORE /
AFTER, preserving the guest console; it is a diagnostic, not a PASS assertion.
Evidence: `temp/q185-memory1/boot/screen.log`, original console transcript
`/tmp/zedbsd-q185-memory1.log`, full install `temp/q185-install11/`.

Inspection finds clean VM reclaim excludes active/referenced objects; a
coherent read retains outer file_io ownership across the read as well as its
inner operation. Optional VM fill already has an uncached backend fallback,
but that backend also needs shared-budget BIO allocations. Shared-credit
starvation is proven by counters; the exact ownership/admission correction
still needs a focused regression. Do not simply remove operation/reference
exclusions: fault waiters and mapping/publication lifetimes require analysis.
No production VM/cache change or weakened swap verification was made here.
Resume with this short reproducer before another full installer run.

Final q185 amd64 disk-image build passed after the common Noct changes
(`/tmp/zedbsd-q185-amd64-build.log`). The PC98 build -12 also passed.
This is build evidence, not an amd64 graphical runtime rerun.


## q186 shared-credit recovery

Added a bounded clean-page retirement API for the outer read's pinned cache
identity. It retains the inode/content leases and refuses other resident-page
users, transitions, mappings and held/dirty/busy/pinned pages. Prefetch ownership
is counted separately because those operations use private frames and publish
under the object lock. This permits clean resident-page retirement without
invalidating unpublished prefetch storage. One read retry follows actual
retirement; no blanket relaxation of the existing global clean reclaimer.

Host runner: `ASAN_OPTIONS=detect_leaks=0 sh
plan/ws025/tests/run-pinned-cache-host.sh`.
Normal and ASan/UBSan pass (`/tmp/zedbsd-q186-cache-host3.log`). LeakSanitizer
itself cannot run under this environment's ptrace sandbox; no leak-check claim.
The fixture uses current production file/VM/cache code, with explicit existing
host service overrides for the formerly separate VM/scheduler/I/O components.
The legacy aggregate file-cache runner still fails its old formatter-close
reference expectation and is not claimed passing. The focused runner tests
actual shared-credit demand in the backend, 256 KiB reads through a 64 KiB
cache, held-page and second-reader exclusion, and pending prefetch ownership.
`CACHE_READ_NO_RETRY=1` is a negative control: it disables the new file-layer
recovery reference and fails the transfer assertion as expected.

The initial sole-owner implementation passed the host case but failed the
actual PC98 read. Runtime trace showed flags=32 (cache), mappings=0, active=2,
refs=3. The revised host case retains a prefetch alongside the complete read.
Temporary kernel diagnostics have been removed. q186-memory-trace2 used an
incorrect physical translation and its zero diagnostic values are invalid;
trace3 used the actual PC98 0x80000000 direct-map base and obtained the above
ownership values. Neither diagnostic run is acceptance.

PC98 build -2 and **q186-memory2** pass: the precreated 64 MiB swap validates
all 16383 slots under the real graphical frontend, two IDE disks and 64 MiB
RAM. Target remains 16777216 bytes; final pending=0 and BIO component-cap /
physical failures=0/0. Optional shared-credit refusals are expected under this
pressure (25992); they no longer make this read fail. Evidence:
`temp/q186-memory2/boot/screen.log`, `/tmp/zedbsd-q186-memory2.log`.

Full normal-path installation now runs as `temp/q186-install1`; destination-only
boot and the final installer result are still pending.


## Final acceptance — PASS

`temp/q186-install1/result.json` reports PASS. The real public graphical
installer completed on PC98 486 / 64 MiB / CoreGraph RGB24 with two IDE HDDs.
The source-free second boot attaches **only the installed target**. Its console
records root login and `uname -a` with `pc98 i386`; boot logs resolve private
rootfs.img/data.img overlay and activate swap0 with 16383 slots. This satisfies
the final user criterion. KEEP.TXT and independent kernel/rootfs hashes also
passed in the already-running normal-path fixture; both shutdowns reached the
checked platform halt. No further abnormal-case installer campaign is required.

- Actual completion screenshot: `temp/q186-install1/install/complete.png`.
- Actual copying screenshot, shown to user: `temp/q186-install1/install/rootfs-progress.png`.
- Installed boot/login: `temp/q186-install1/installed-boot/screen.log`.
- Complete run transcript: `/tmp/zedbsd-q186-install1.log`.
- Scope: pre-existing PC98 FAT16 and working bootstrap sectors; no GPT/native
  PC98 installation or blank-disk bootstrap provisioning. Physical PC-9821V13
  pre-text boot failure remains WS025-p032 and is not cleared by this QEMU run.

WS019-p050 is cleared. amd64 installer acceptance remains q182–q184; the PC98
addition does not reopen that completed normal-path installer campaign.

Final common-kernel build gates passed for amd64 and PC/AT as well as the
PC98 image accepted above: `/tmp/zedbsd-q186-amd64-build.log`,
`/tmp/zedbsd-q186-pcat-build.log`, `/tmp/zedbsd-q186-pc98-build2.log`.
No extra installer abnormal/near-normal runtime tests followed user acceptance.

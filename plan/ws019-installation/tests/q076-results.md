# Q076 evidence and residual validation

Recorded: 2026-09-05

Parent: [test index](README.md); [finished q076](../../queue.md).

## Delivered implementation

- Basic disk geometry/registration query and visible mount-list query. No GPT
  records cross the administration UAPI. Mount membership is captured under
  the namespace lock; referenced paths are reconstructed outside it using
  bounded getcwd semantics, not a transaction across concurrent renames.
- 512/4096-byte devfs block I/O, existing checked 64-bit offsets and fsync.
- Privileged whole-disk `BLKREREADPART`: backing-claim exclusion plus registry
  admission gate; any mounted/open child prevents reload, including ro/root
  and unchanged extents. Preparation precedes one atomic registry replacement;
  fresh device registrations replace old objects. No force or table writer
  exists in the kernel administration path.
- `/sbin/diskpart` userspace GPT/primary-MBR parsing and confirmed existing-table
  add/delete. GPT writes backup then primary with flush/read-back at each
  stage. Stale originals abort; failures distinguish no-write, potentially
  partial-write, and persisted-but-not-reloaded. No crash-atomicity claim.
- No detected-partition `/diskN` auto-mounts. Configured boot/root/overlay/swap
  source handling remains; auxiliary filesystems require explicit mounts.

No production media, installer, formatters, UEFI Boot variables, partition
resizes/moves, or whole-disk initialization were executed.

## Host and build evidence

Run from the repository root (never aggregate `make check`):

```sh
sh plan/ws019-installation/tests/run-storage-foundation-test.sh
sh plan/ws019-installation/tests/run-diskpart-table-test.sh
make -j16 sysroots
make -j16
make -j16 ZEDBSD_CONFIG=plan/ws021-llvm-toolchain/tests/config-pcat.mk
```

| Gate | Result |
| --- | --- |
| Production-linked disk/devfs/mount/reload foundation | 20,376 checks PASS ordinary and ASan/UBSan; 1,000 reload cycles; amd64/i386 UAPI syntax/layout PASS |
| Independent sparse-image production userspace parser/writer | 1,722 checks PASS ordinary and ASan/UBSan; GPT 512/4096, primary MBR, >4-GiB backup, malformed CRC-valid structures, disagreeing copies, short I/O/flush/read-back faults, metadata-only golden round-trips |
| Production CLI with memory-only syscall/input adapters | 182 checks PASS ordinary and ASan/UBSan; syntax, confirmation, non-block/partition/ro targets, stale live extents, deletion busy pre/post-confirmation, write/flush faults, reload EBUSY/EIO and exits 0/1/2/3 |
| Maintained build | Final amd64 and i386 PC-AT `make -j16` PASS; no PC-98 runtime or build claim |
| WS004 devfs range, partition publication, strict GPT | Ordinary/sanitizer/analyzer PASS |
| WS006 dynamic cdev/devfs lifetime | Ordinary/ASan/UBSan/analyzer PASS |
| WS016 backing claims | Production claim fixture PASS |
| WS018 disklabels / filesystem identity | Existing label fixture PASS; filesystem identity 110 checks and owner audit PASS |

The foundation fixture substitutes memory I/O, claim results and deterministic
admission interleavings; it is not a full mounted-filesystem concurrency test.
Real backing-claim code has its separate regression. The ASan foundation
runner disables ASan global-root retention to allow unused exported vtables
to be garbage-collected; stack/heap/access instrumentation remains enabled.
The dynamic-devfs and identity fixtures needed new dependency stubs; the
identity fixture's creation stub aborts if invoked, never fabricates success.

Build setup failure before these passes: new public headers were not yet in
the generated sysroots. Explicit `make -j16 sysroots` resolved that prerequisite.
The host CLI's GCC truncation diagnostic was resolved with an explicit bounded
format precision, preserving the existing validated device-name limit.

## Bounded QEMU attempts — not an overall PASS

All used `qemu-system-x86_64`, fresh disposable boot/OVMF-VARS copies and
independently generated auxiliary images, with 120-second boot and 600-second
whole-cell ceilings. Artifacts are ignored under `../temp/`, not committed.

| Cell | Observed result / diagnosed stop |
| --- | --- |
| `q076-idle-01` | Login and mount/list/show passed. Existing boot policy auto-mounted auxiliary FAT at `/disk2`; reload correctly returned EBUSY. Extra NVMe controller was also unsupported by the existing driver. No writer acceptance. |
| `q076-combined-02` | One NVMe plus IDE auxiliary device. EBUSY again correct. Harness read `$?` on a new shell input line and expected 1; current shell resets that value each line. No writer acceptance. |
| `q076-explicit-03` | Auto-mount-removal kernel booted overlay/swap to login. Helper rootfs was copied to GPT slot 1 (ESP), not the separate configured payload; helper was not found. Harness only; production shell unchanged. |
| `q076-explicit-04` | Payload resolved from its unique existing rootfs and helper verified before boot. Mount query showed no `/diskN` or auxiliary FAT mount; auxiliary `/dev` node present; overlay and swap0 (16,383 slots) active. Idle reload, GPT add/delete and MBR add/delete all returned actual exit 0 through test-only waitpid observer. Explicit `mount ... /mnt/q076` returned EINVAL: existing public mount implementation accepts only root-level destinations. Cell stopped before mounted-add/root/reboot checks. |

After the final stopped cell, read-only host audit verified **complete-image**
round-trip equality after GPT and MBR add/delete, including filesystem payload
and all non-table sectors:

- GPT SHA-256: `b73336a9077d5e0fc43c4a5682c01ee5c412652290a69a4f47c6d66bf1047786`
- MBR SHA-256: `003a9f407964e49828eef01714a8c4e2e3262e3762e0740509b52d5bdbb38b82`

The runner always stops its QEMU child on failure. No guest was left running.
Failure logs remain in each cell directory as `guest.log`, `commands.log`,
and `qemu.log`; no failed cell receives a fabricated PASS result.

## Concrete resume contract

P002 and p003 are completed. P010/p011/p012 remain **uncleared**, despite their
implemented code and partial successful verification. No fifth launch occurred.
The maintained runner is corrected to `/q076`; no production mount-path or
shell fix was made. A new finite Queue approval is required for the remaining
runtime window, suggested at most two launches / 30-minute review:

```sh
make -j16
make -j16 -f Makefile -f plan/ws019-installation/tests/storage-qemu.mk ws019-storage-qemu-fixture
# Only after the next Queue is approved; output must not exist:
python3 plan/ws019-installation/tests/run-storage-qemu.py combined plan/ws019-installation/temp/q076-resume-01
```

The helper image contains ordinary production packages plus only a waitpid
exit observer; it is not installed in the ordinary production image. The runner
verifies that helper and its copied payload before launching QEMU.

Remaining acceptance: explicit ro/rw FAT mounts at `/q076`; no-op/unchanged
whole-disk reload EBUSY; mounted addition persists but exits 3, old mounted
partition usable and new device absent; actual boot-disk reload EBUSY; reboot
discovers the added partition with auxiliary FAT still unmounted. Preserve
non-table bytes and production input hashes. Failures must remain explicit;
do not use force, silently extend the cell budget, or call this WIP cleared.

## Format reference

GPT field/CRC/layout decisions refer to the primary
[UEFI 2.10 partition format specification](https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html).
This implementation intentionally limits edits to existing matching 92-byte
headers / 128-byte entries and at most 16 active published partitions.

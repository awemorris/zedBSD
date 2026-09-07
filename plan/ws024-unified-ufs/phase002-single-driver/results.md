# WS024 p002 implementation and evidence

2026-09-07, q101; completed: core driver integration plus producer-mounted feature, remount
and reboot acceptance pass. Full WS acceptance/retirement belongs to p004.

One `src/drivers/fs/ufs/` owner and `include/kern/ufs.h` now carry the former
64-bit codec, including WS025 data runs, metadata views and persistence epochs.
All six kernel manifests use one source list; VFS registers only `ufs` and uses
the unified root marker. Superseded source files remain for the p004 retirement
gate but are not normal kernel link inputs. Unified I/O events are appended;
published historical event numbers remain unchanged.

Decoded inode size and block count are checked against each active ABI before
inode allocation. Disk pointers remain 64-bit on both ABIs. Superblock decoding
rejects invalid shift counts, inconsistent powers, oversized inode populations
and invalid final group geometry. Loop attachment checks the original inode
size before narrowing, retaining its existing positive signed-32-bit limit.

Evidence under `../temp/` (disposable logs, not committed):

- `p002-host-final`: ordinary and ASan/UBSan production-linked metadata,
  run, view and consistency fixtures pass. Allocation/truncate audit has
  6,595 checks; view variant 6,635; journal/snapshot consistency 45.
- `p002-width-final`: actual freestanding LP64 and ILP32 executables pass
  high direct/indirect fragment addressing and little/big-endian ABI limits.
  The broad libc/pthread i386 fixture could not build because host i386
  development headers are absent; it is not recorded as passed.
- `p002-boundaries`: both ordinary and sanitizer superblock matrices pass
  44 checks each. Actual FAT/loop attachment rejects a 4 GiB + 512 byte
  inode with EFBIG without acquiring a backing claim; existing mapped I/O,
  refusal, flush failure and detach stories also pass in both variants.
- `p002-build-final.log`: supported amd64 `make -j16` build passes (exit 0). No native runtime is authorized for the intermediate
  kernel/old-UFS1-generated-image combination.

Remaining evidence: coherent producer-generated images, integrated quota and
extended-attribute persistence, full namespace/lifetime matrix, supported
32-bit builds, boot and retirement gates. The 45 consistency checks do not
establish quota or extended-attribute remount coverage. P003 can begin once
this driver interface is built, while p002 stays open for those feature gates.
Physical acceptance follows the user's instruction: user-accepted, no agent
physical execution claimed.

## Integrated recovery finding

The first mounted-feature QEMU run passes 51 public-syscall checks, including
xattrs, persisted user/group quota, namespace identity, active snapshot unmount
refusal, old data/xattrs via a mounted snapshot, deletion and live remount.
Its reboot then fails to mount the ordinary overlay upper with EROFS. The prior
UFS1 implementation deliberately accepted an unclean marker after structural,
allocation-summary and root validation; the copied UFS2 implementation instead
rejects fs_clean=0 without a journal. Normal shutdown syncs the private upper but
does not unmount it, so that inherited restriction breaks ordinary reboot.
Carry the existing validated UFS1 recovery policy into the single owner; retain
all structural/summary/root checks and read-only-device refusal. Do not mark a
still-mounted filesystem clean on every fsync. Rerun the complete two-boot gate.
This is required feature preservation, not evidence of arbitrary crash recovery.

Also replace the CG-buffer allocation failure's bare state free with the common
state destructor, so a previously allocated optional snapshot map is released.

## q101 completion evidence

The open producer-mounted feature gate is resolved by p003's real QEMU syscall
fixture: 51 run/remount checks plus 15 reboot checks pass in
`temp/p003-features-recovery`. Target mkfs/native root/overlay reboot passes the
maintained WS019 combined cell. All three configured builds pass. The common
state cleanup and validated reopen fixes are included in the final amd64 build.
P004 owns the complete WS matrix, including deeper quota limit enforcement,
malformed xattr inputs, remaining namespace faults and retired fixture ownership;
this phase does not claim those final rows from its narrower smoke coverage.

# Unified UFS implementation contract

Decision: q100 / ws024-p001, 2026-09-07. The public name is `ufs`.
This contract freezes the implementation transition; it does not claim the
producer/consumer changes have already been made.

## Format and limits

Use the current UFS2 disk codec, including magic 0x19540119, primary superblock at
byte 65536, 256-byte dinodes, 64-bit fragment addresses, file sizes and block counts,
12 direct plus three indirect levels. Do not invent a new magic or interpret a
UFS1 inode as this layout. Rename the implementation/API/constants to `ufs`/`UFS`
while preserving the binary layout and explicit endian decoding.

The canonical generated profile uses 8192-byte blocks, 1024-byte fragments, eight
fragments/block, 1024 pointers/indirect block, 32 dinodes/block and 256 inodes/CG.
Per-CG metadata remains sblk=64, cblk=72, iblk=80, dblk=144 fragments. Place the
inode/free bitmaps without overlap and derive their sizes with checked arithmetic.
Use at least two CGs; increase their count when needed to keep each CG bitmap
within its filesystem block. No blanket widening of bounded per-group indices,
sector counts or directory inode numbers is required.

| Boundary | Contract |
| --- | --- |
| Disk codec and generic BIO block address | Unsigned 64-bit on every CPU; validate shifts, additions, range subtraction and sector conversion before I/O. |
| Inode number in a directory entry | Existing 32-bit format field; reject geometry whose usable inode population cannot be represented. |
| LP64 VFS/file offsets | Signed 64-bit; reject disk inode size/block-count values that cannot fit the existing public types. |
| Existing ILP32 VFS/user ABI | `off_t`, `blkcnt_t` remain signed 32-bit in the current ABI. Reject oversized inodes/offsets explicitly, never truncate their 64-bit disk values. One shared 64-bit disk driver still serves this ABI. Widening the syscall/stat ABI is separate compatibility work, not silently bundled here. |
| Current loop backing | Existing 2 GiB bound remains an explicit checked limit; fix its narrowing-before-validation bug. Do not advertise 64-bit loop capacity from the disk codec alone. |
| FAT backing | Preserve its existing 32-bit file-size and geometry constraints; they do not constrain direct UFS disk addresses. |
| Initial target formatter | Fixed existing file, minimum 4 MiB, integral 1024 bytes, maximum 2147482624 bytes (largest fragment-aligned positive ILP32 file). Check before reservation/mutation; this is a producer limit, not a UFS disk-address limit. |
| Host image builder | Same canonical geometry and producer bounds for reproducible target/host comparison. Large-address driver acceptance uses sparse mock storage, not multi-gigabyte allocated images. |

The current UFS2 driver casts its decoded inode size directly to off_t; p002 must
replace that with a checked conversion. The loop attachment currently narrows
inode size to uint32_t before its limit check; p002 must check the original value.
These are required transition fixes, not accepted behavior. Keep readonly native
root and writable overlay/data behavior on all three supported x86 configurations.

## Optional persistence features and generated profiles

Keep the existing optional-tail discovery contract. The filesystem's declared
fragment count excludes all tail regions; allocation bitmaps cannot allocate them.
The current `ZUJ2` locator describes its following journal sectors, uses version 2
and checksum rules, and records the filesystem-end sector. A following `ZSL1`
locator independently describes snapshot storage and the original filesystem end.
The sole `UFSJ`/`UFJC` journal codec is the multi-extent version 2 introduced by
WS025 p021 under the user decision to remove unreleased v1 compatibility. Snapshot
wire codecs remain unchanged. Recognized obsolete journal locators are refused.
Validate all counts, ends, checksums and nonoverlap before publishing the mount.

The common builder/formatter must support constructing and validating these
regions as an explicit persistence profile, including clean initialization and
recovery fixtures. Preserve existing opt-in behavior during write-through driver
consolidation: ordinary generated roots/data do not silently acquire a new
per-write journal policy in this WS. WS025 p021 selects/initializes its required
journal profile before enabling metadata write-back. The profile must exist and
be tested in WS024; it must not remain a future stub. Expose it as the optional
`--profile=journal-snapshot` argument to `mkfs -t ufs` and matching host builders;
plain `mkfs -t ufs FILE` retains the ordinary write-through profile. Select distinct
formatter callbacks for each profile within the existing descriptor-owned format
transaction, without global mutable formatter state.

For the feature acceptance profile, reserve a 256-sector journal plus its locator,
then a 2049-sector snapshot region plus its locator; round the filesystem end down
to a fragment boundary and leave any final padding unallocated. The snapshot region
contains at most 1024 before-image records. No snapshot is active initially.
A full snapshot store must fail safely; do not claim it can snapshot unlimited writes.

Overlay `.zovl0` and `.zovl1` remain ordinary allocated files with their own existing
record/checkpoint format, not aliases for either tail locator or filesystem journal.
Data-image and target-mkfs initialization retains them. Generic extended attributes
and `system.zedbsd.quota` remain inode-owned data, with independent round-trip tests.

## Names, image handling and switch order

Final names: `mount -t ufs`, `mkfs -t ufs FILE`, blkid type `ufs`, one registration
`ufs_filesystem_type`, one public `include/kern/ufs.h`, one production owner
`src/drivers/fs/ufs/`. Root marker becomes `zedBSD ufs root v1\n`. Old UFS1/UFS2
public type/CLI spellings are rejected after the transition; do not keep a second
permanent driver or formatter alias.

Existing valid UFS2-format images retain their binary codec compatibility and are
recognized as UFS; an old root marker does not silently become the new generated
root contract. UFS1 images are rejected without mutation. Migration of retained
user data is export with the old system, create a separate new image, import and
verify; no automatic in-place conversion is authorized or implemented.

Before replacing generated `build/data.img`, root/architecture images or packed
boot images, retain opaque copies and hashes under this WS's ignored temp area.
Regenerate from the existing declared build inputs. Never treat unknown external
or real-device data as disposable; do not read `.internal/` to locate images or
credentials. No real device is rewritten by this WS.

P002 builds the shared driver and checked-width behavior. P003 switches all
producers, registration/root detection and build manifests as one coordinated
build transition. Do not run a new kernel against an old generated root/data pair.
P004 removes superseded production paths after preserved-feature, width, formatter,
boot, persistence and source/manifest checks pass. Temporary files during this
transition are bounded by p004; historical Queue/results documents retain their
original UFS1/UFS2 wording and evidence.

## Confirmed source inventory

- Driver/registration: both `src/drivers/fs/ufs1` and `ufs2`, their public headers,
  generic VFS registration and UFS1-only root-marker helper.
- Target formatter: `userland/base/mkfs/ufs1-format.[ch]` and main's `ufs1` dispatch;
  retain descriptor-owned formatting reservation, identity/no-follow checks,
  fixed size, flush/readback verification and refusal behavior.
- Normal host backend: `tools/build/zedimage-host.c`, currently a UFS1 codec;
  Noct `ufs1_format.noct`, architecture and data-image wrappers.
- Python producers/checkers: UFS2 inherits tree/CG code and checker behavior from
  UFS1-named files. Extract format-neutral ownership and remove the old disk codec;
  renaming only the UFS2 subclass would not complete consolidation.
- Platform/native-root consumers: Makefile and platform vmunix manifests, BIOS
  packed images, ARM64/RPi4 and SPARC image/check paths, blkid, root discovery,
  reusable host/native fixtures and active installer plans.

Full read-only inventory and source hashes: `temp/p001/consumer-inventory.json`
and `temp/p001/source.sha256.json`. No implementation test is claimed by that inventory.

Statistics keep existing published numeric event IDs as historical ABI entries;
append unified UFS read/write/content event IDs and update current fixtures to the
single owner. Retired event names are not a second active filesystem implementation.
Fixture migration inventory is `temp/p001/fixture-inventory.json`; historical
results remain untouched, while runnable tests/recipes must resolve real sources.


WS025 p021 staged extension: a separately selected journal v2 profile will carry
multi-extent redo groups. The core uses UFSJ descriptor version 2 and a commit that
binds the complete descriptor; home-volume bounds are explicit at initialization.
The v1 profile remains supported. No existing locator or mount is automatically
upgraded by introducing the core API. The formatter/locator profile selector and
migration/refusal rules must be implemented before production v2 admission.
Detailed layout/ordering: ../ws025-io-memory-cache/phase021-ufs-metadata-writeback/transaction-design.md.


Superseding WS025 p021 user decision: journal version 1 need not be preserved
because the OS has not been released and has no users. The current journal
contract is ZUJ2 locator/version 2 plus multi-extent UFSJ/UFJC version 2. One
codec handles single and multiple extents. Removed v1 compatibility is intentional;
recognized obsolete ZUJ locators are refused rather than mounted without recovery.
All three current format producers generate the new locator. Earlier version-1
statements above record the WS024 baseline and no longer constrain journal work.
Snapshot ZSL1/ZSN1 is a separate unchanged format, not a retained journal v1 path.

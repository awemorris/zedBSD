# WS003 Phase 014: multi-source swap activation

Last updated: 2026-08-27

WSID: `ws003`

Phase ID: `p014`

Combined ID: `ws003-p014`

Status: Completed (`q015`, 2026-08-27)

Parent: [WS003](../ws.md)

Contract: [kernel boot parameters](../../../docs/reference/kernel-boot-parameters.md)

Shared tests: [WS003 test index](../tests/README.md)

## Objective

Activate `swap0`--`swap3` as one deterministic system pool using either files
on named boot filesystems or explicitly selected raw swap partitions.

## Scope

- zero to four sparse swap sources;
- `bootN:PATH` file sources and common block-device selectors;
- activation of the existing FAT-file extent path;
- a direct block-device source that bypasses pageable buffer-cache memory;
- mandatory swap signatures for both files and partitions;
- backward reading of existing `ZEDSWAP1` 32/64-MiB files;
- a versioned 64-bit-capacity swap-header successor;
- one aggregate backend whose slot ranges follow swap-number order;
- atomic validation/activation and complete failure unwind;
- aggregate statistics, ordered flush, and ordered shutdown; and
- focused concurrent allocation/I/O/lifecycle fixtures.

## Non-goals

- dynamic swapon/swapoff syscalls or commands;
- priorities other than numeric `swap0`--`swap3` order;
- striping, mirroring, encryption, hibernation, or bad-block remapping;
- accepting unsigned/raw arbitrary partitions; or
- silently migrating a page after source I/O failure.

Boot source preparation is serialized and uses private FAT mounts. A later
runtime swapon/swapoff Phase must introduce a backing-object claim shared with
ordinary inode write/truncate and loop attachment, a disk/superblock writable
claim that covers aliases through separate mounts, and VM commit-limit
recalculation. The boot-only inode marker in this Phase is deliberately not
claimed as that runtime synchronization mechanism.

## Design

Keep the existing VM-facing single `system_backend` interface by making it an
aggregate backend. Each validated source contributes one contiguous range of
global slots in numeric order. The existing first-free scan therefore fills
`swap0`, then `swap1`, `swap2`, and `swap3` without encoding backend identity
into the VM's `uint32_t swap_slot`.

Aggregate read/write operations map a global slot to its immutable source and
local slot. File and block sources both reserve their first 4096-byte page.
The raw backend performs direct disk I/O. Every source is opened and validated
before the aggregate is published; any failure destroys all prepared sources
and leaves the system backend unset.

The successor header retains a 64-byte checksummed header and uses 64-bit byte
and slot capacities. Validation checks the backing object's actual length
rather than trusting header capacity. The host image tool emits the successor
format while the kernel continues accepting valid `ZEDSWAP1` images.

USB-backed swap must also make forward progress after reclaim has consumed the
last allocatable physical page. Each xHCI controller therefore allocates one
embedded transfer request and one 8-KiB-aligned, 8-KiB coherent bounce reserve
before the controller runs. The controller's existing single-flight transfer
rule makes that one reserve sufficient for the current page-sized storage I/O
and its BOT envelope. Transfers of 8 KiB or less use the reserve without a
heap or DMA allocation; larger generic USB transfers retain the existing
dynamic request/bounce path and are outside this page-sized guarantee.

USB storage allocates persistent control, bulk-in, and bulk-out URBs during
attach, before the disk can back swap, and serializes their reuse with the
storage mutex. `drv_usb_urb_wait_reusable()` waits for both terminal status and
the HCD ownership hold to drop before the next setup. xHCI releases the
reserved request before terminal publication, while the atomic
`completion_busy` counter prevents detach/stop quiescence from observing a
false zero across overlapping or reentrant completion publication. Cancel and
failed-start paths retain ownership until their recovery boundary; detach and
controller stop release the persistent URBs and reserve only at their existing
quiesce boundaries. The controller reserve is freed only after `HCHalted` and
no active/reserved/completing request remains.

## Work packages

1. Separate current FAT swap source preparation from publication of the sole
   system backend.
2. Implement source-array mapping and an aggregate backend with deterministic
   first-fill allocation.
3. Implement direct raw-partition page I/O and balanced disk ownership.
4. Define, generate, checksum, and validate the 64-bit-capacity successor
   header while preserving `ZEDSWAP1` compatibility.
5. Reject duplicate file/inode and duplicate block-device sources, including
   aliases through different selector spellings.
6. Resolve a native root before swap publication and reject any raw source
   whose physical leaf range overlaps it; do not reject a file-backed source
   merely because it resides on that filesystem.
7. Implement atomic activation, ordered flush/shutdown, aggregate statistics,
   and error diagnostics naming `swapN`.
8. Wire parameter-selected activation into VFS after boot slots are resolved
   and before VM commit/reclaim can allocate swap.

## Verification

- Add BR-T45 fixtures for zero/one/four sparse sources, numeric fill order,
  duplicate aliases, malformed headers, length mismatch, read-only sources,
  exact boundary slots, direct block offsets, partial activation unwind,
  concurrent allocation/free/I/O, ordered flush, shutdown, and raw/native-root
  physical-range collision rejection.
- Verify legacy `ZEDSWAP1` and new header images separately.
- Boot disposable images with file-only, partition-only, and mixed two-source
  configurations and prove nonzero aggregate statistics plus actual page
  swap-out/swap-in.
- Run VM reclaim/commit/resource regressions, `make -j16`, and
  `git diff --check`.
- Do not run `make check` or use `.internal/`.

## Completion conditions

- every valid sparse `swap0`--`swap3` combination activates atomically;
- invalid or duplicated sources leave no active backend or leaked ownership;
- a raw source overlapping native root is rejected before publication;
- allocation uses numeric first-fill order and I/O returns to the same source;
- FAT-file and raw-partition sources pass direct I/O tests;
- aggregate stats/flush/shutdown are correct;
- existing generated swapfiles remain accepted; and
- BR-T45, VM regressions, build, and QEMU swap tests pass.

## q015 execution evidence

BR-T45 passed its production-shared header, aggregate mapping, concurrency,
failure-unwind, flush, and shutdown coverage. The affected VM and storage
regressions and production build gates also passed.

The USB-backed forward-progress correction passed the following focused
BR-T46 cells. Each `results.tsv` records `pass`, and the corresponding
`guest-logical.log` ends in the swap exercise's full readback-integrity marker
with nonzero page-in and page-out counters:

| Cell | Evidence directory | Final page-in / page-out |
| --- | --- | --- |
| amd64 UEFI file swap, repeat 002 | `plan/ws003-bringup/temp/q015-br-t46-reserve-file-focused-002-uefi` | 1767 / 3533 |
| amd64 UEFI file swap, repeat 003 | `plan/ws003-bringup/temp/q015-br-t46-reserve-file-focused-003-uefi` | 1767 / 3533 |
| amd64 BIOS raw swap | `plan/ws003-bringup/temp/q015-br-t46-reserve-raw-focused-001-bios` | 2755 / 5509 |
| amd64 UEFI raw swap | `plan/ws003-bringup/temp/q015-br-t46-reserve-raw-focused-001-uefi` | 1748 / 3495 |
| amd64 BIOS mixed file/raw swap | `plan/ws003-bringup/temp/q015-br-t46-reserve-mixed-focused-001-bios` | 2799 / 5597 |
| amd64 UEFI mixed file/raw swap | `plan/ws003-bringup/temp/q015-br-t46-reserve-mixed-focused-001-uefi` | 1792 / 3583 |

All eight existing affected host regressions also pass:
`xhci-capability-mmio-test`, `usb-hcd-unregister-test`,
`xhci-control-ep0-reset-test`, `xhci-cancel-command-test`,
`dma-allocation-lock-test`, `xhci-model-test`, `usb-storage-scsi-test`, and
`usb-urb-publication-test`. BR-T39 `usb-storage-flush-test` also passes against
the corrected USB-storage path.

The authoritative post-review run at
`plan/ws003-bringup/temp/q015-br-t46-final-007` passed all 31 BR-T46 cells. All
10 positive file, raw, and mixed swap cells ended with full anonymous-page
readback and `OBJECT-SHARED PASS`; final page-in counts were 1748--2799 and
page-out counts were 3495--5597. The PC/AT native-root/raw-swap alias cell also
passed by rejecting the alias before publication. No BOT/storage fatal marker
was present. The authoritative `results.tsv` SHA-256 is
`d64dfbcc76d3c0f8e86f27ba86a7280391ca7a4e86fe0666c404c5d1f8a7d8cf`.

The final ownership correction reserves one xHCI request and 8-KiB coherent
buffer, allocates persistent USB-storage URBs, and prevents synchronous URB
reuse until the terminal state and HCD ownership release are both visible.
xHCI stop now gates and drains URB submissions, non-URB HCD callbacks, command
and completion activity, and checked IRQ removal before releasing any DMA. A
failed barrier retains the complete resource set. The added timeout-retention
and checked-IRQ-`EBUSY` host regressions pass; legacy EHCI/UHCI migration is
tracked outside q015 by `ws004-p009`.

## Dependencies and handoff

Depends on `ws003-p011` through `p013`. BR-T46's file, raw, and mixed swap cells
are shared p014/p015 runtime evidence; their authoritative page-out/page-in and
readback results complete this Phase without making p015 a source-code
dependency of p014.

## Reconsideration boundary

Stop if aggregate slot counts exceed the existing 32-bit VM slot identity, if
the header migration cannot preserve current images safely, or if shutdown
cannot drain every source without a VM lifecycle redesign.

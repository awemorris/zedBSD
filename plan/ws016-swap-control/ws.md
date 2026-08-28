# WS016: runtime swap control

Last updated: 2026-08-28

WSID: `ws016`

Status: Complete (`q021`)

Parent: [master plan](../master.md)

Last verified Phase: `ws016-p004`

Resume point: no planned Phase remains; extract a new requirement before
resuming WS016.

Shared tests: [WS016 test index](tests/README.md)

## Goals

- Add native `/sbin/swapon` and `/sbin/swapoff` administrative commands.
- Add and remove signed file-backed or raw-partition swap sources while the
  system is running without invalidating pages already stored in another
  source.
- Preserve the `swap0`--`swap3`, `bootN:PATH`, UUID/PARTUUID, header,
  duplicate-source, native-root-alias, direct-I/O, and VM-integrity contracts
  completed by `ws003-p014` and `ws003-p015`.
- Make capacity changes immediately and safely visible to VM commit
  accounting and aggregate `/dev/system` statistics.

## Standards status and objective

`swapon` and `swapoff` are not specified by POSIX.1 or SUSv4 Issue 7. They are
zedBSD extensions with the familiar BSD/Linux administrative operation: a
privileged user names a swap backing object to enable or disable. The initial
CLI is intentionally smaller than either system and makes no source- or
option-level compatibility claim.

The implementation reuses zedBSD's existing signed `ZEDSWAP1`/`ZEDSWAP2`
sources rather than accepting arbitrary writable storage. Runtime control is
provided through versioned `/dev/system` ioctls; no falsely POSIX-named libc
function or new public syscall is introduced by this WS.

## Baseline and design constraints

- The current kernel prepares at most four boot-selected sources and publishes
  one aggregate `struct swap_backend` exactly once.
- Global VM slot numbers are contiguous offsets into that immutable aggregate.
  Rebuilding or compacting those offsets while pages are swapped would direct
  page-in I/O to the wrong source.
- Runtime regular-file activation needs stronger exclusion than the existing
  boot-only `INODE_SWAPFILE` marker: ordinary write/truncate/rename/unlink,
  loop attachment, writable aliases through another mount, and source teardown
  must share one backing-object claim.
- `swapoff` cannot destroy a source merely because no I/O is currently active.
  It must first stop new allocations to that source and make every page stored
  there resident again.
- `vm_commit_init()` currently snapshots swap capacity once. Runtime capacity
  changes require an atomic update that rejects removal when the remaining
  physical-plus-swap limit is below already reserved commitment.

## Fixed runtime model

- At most four sources are active, including boot-selected sources. Source IDs
  are 0 through 3. A boot `swapN` owns ID N; runtime `swapon` takes the lowest
  unused ID.
- A VM swap-slot token uses a stable source/local encoding. Bits 29--30 hold
  the source ID, bits 0--28 hold its local page index, and bit 31 remains zero;
  `UINT32_MAX` therefore remains `SWAP_SLOT_NONE`. One source is limited to
  2^29 pages (2 TiB at 4096 bytes), and the four-source aggregate remains
  representable by current counters.
- Allocation visits active source IDs in numeric order. Removal never shifts a
  surviving source's tokens. A freed source ID may be reused only after its
  allocated slots and in-flight operations have reached zero.
- Runtime source operands use the existing boot-source grammar:
  `bootN:PATH`, an absolute namespace path for a regular file, `/dev/NAME`,
  `UUID=...`, or `PARTUUID=...`. Canonical inode/disk identity, not spelling,
  decides duplicate activation and `swapoff` matching.
- File-backed activation initially supports only files for which the kernel
  can pin and validate a non-pageable direct-I/O extent map. The present FAT
  implementation qualifies; inventing a UFS extent contract is outside this
  WS and unsupported files fail with `EOPNOTSUPP`.
- Every source requires a valid existing `ZEDSWAP1` or `ZEDSWAP2` header. These
  commands do not format swap.
- `swapoff` marks the source draining so new allocations skip it, then pages in
  all private VM backing stored in that source. Reclaim may use other active
  sources while draining. Insufficient commitment or memory returns `ENOMEM`
  and returns the still-intact source to active state; already paged-in data
  remains resident.
- Source removal succeeds only after allocated and in-flight counts are zero,
  flush completes, the VM commit limit is reduced safely, and every backing
  claim can be released. Failure retains a usable source and its ownership.
- Only effective UID 0 may enable or disable swap. Source enumeration is
  readable without privilege.

## Public UAPI and command surface

Phase 002 adds versioned structures to `<zedbsd/system.h>` and three
`/dev/system` ioctls: add source, remove source, and enumerate source. Control
structures contain `version`, `struct_size`, zero-only flags/reserved fields,
and a bounded source string. Enumeration returns source ID, active/draining
state, header version, total/used pages, UUID/label when present, and the
diagnostic source spelling. Unknown versions, sizes, flags, or unterminated
strings fail without mutation.

The initial commands are exactly:

```text
swapon [--] SOURCE...
swapoff [--] SOURCE...
```

At least one operand is required. Operands are attempted from left to right;
all operands are attempted, exit status is zero only when all succeed, one for
an operational failure, and two for usage. `-a`, priorities, formatting,
hibernation, encryption, and a listing option are not part of the initial CLI.

## Non-goals

- claiming POSIX or SUS utility/API conformance;
- changing the boot-parameter grammar or increasing the four-source limit;
- implementing `mkswap`, `fstab` swap entries, `swapon -a`, priorities,
  striping, mirroring, encryption, hibernation, or bad-block management;
- dynamically creating a UFS swapfile extent ABI;
- silently discarding or redirecting a page after an I/O failure.

## Dependencies

- [`ws003-p014`](../ws003-bringup/phase014-multi-swap/phase.md) owns the signed
  source formats, direct file/raw I/O, boot aggregation, and swap-integrity
  baseline.
- [`ws003-p015`](../ws003-bringup/phase015-x86-parameter-acceptance/phase.md)
  owns the four-platform boot-parameter evidence reused as a regression gate.
- WS001 records the commands and UAPI as zedBSD extensions rather than POSIX
  requirements; WS009 later publishes administrator-facing documentation.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws016-p001` | [Runtime swap manager](phase001-runtime-swap-manager/phase.md) | Complete (`q021`) | Stable source-encoded slots, dynamic add/drain/remove, backing claims, and live commit accounting pass host tests |
| `ws016-p002` | [`/dev/system` swap UAPI](phase002-swap-uapi/phase.md) | Complete (`q021`) | Versioned privileged control and source enumeration pass ABI, permission, and failure-atomicity tests |
| `ws016-p003` | [`swapon` and `swapoff` commands](phase003-swap-commands/phase.md) | Complete (`q021`) | Both `/sbin` utilities implement the fixed multi-operand CLI and are installed in configured images |
| `ws016-p004` | [Runtime swap QEMU acceptance](phase004-runtime-swap-acceptance/phase.md) | Complete (`q021`) | Disposable amd64 UEFI QEMU images prove add, page-out/in, installed-command drain/remove, failure preservation, ID reuse, and boot-swap regression |

## WS completion conditions

- Boot-selected and runtime-added sources coexist without changing surviving
  slot identity or boot behavior.
- Runtime `swapon` validates ownership and publishes capacity atomically;
  duplicate/alias/malformed/read-only/unsupported sources leave no claim.
- Runtime `swapoff` preserves every swapped page, rejects unsafe commitment
  reduction, and releases no source while allocation or I/O remains.
- File/inode, disk, loop, and mount aliases cannot mutate an active backing
  object through a second path.
- Aggregate/source statistics and VM commitment reflect every successful
  transition.
- The installed commands obey the fixed CLI and privilege contract.
- SWAP-T001--T012, affected VM/storage regressions, `make -j16`, and amd64
  `qemu-system-x86_64` acceptance pass without `make check` or `.internal/`.

## Reconsideration boundaries

Stop and return for a new design decision if four stable source IDs cannot be
encoded without breaking persisted VM state, if an active file cannot be
protected from writable aliases without a general filesystem claim, if
source drain needs an unbounded stop-the-world VM walk, or if `/dev/system`
cannot retain source ownership across interrupted control calls. Do not reduce
`swapoff` to destructive detach or declare a runtime file safe using only the
boot-time inode flag.

## Completion result

WS016 completed in `q021` on 2026-08-28. SWAP-T001--T012, the installed native
commands, focused VM/storage/UAPI regressions, and the final six-cell amd64
UEFI QEMU matrix passed. The final p004 result table is recorded in the Phase
book with SHA-256
`94c36cc82625d8150db69269df50d02a03ba8e0f1233f4a4cdcb284b3ed07f14`.

# WS016 Phase 001: runtime swap manager

Last updated: 2026-08-28

WSID: `ws016`

Phase ID: `p001`

Combined ID: `ws016-p001`

Status: Complete (`q021`)

Parent: [WS016](../ws.md)

Tests: [WS016 test index](../tests/README.md)

## Objective

Replace the immutable boot aggregate with a four-source runtime manager that
can add, drain, and remove a source without changing another source's VM slot
identity or weakening the existing signed-source and direct-I/O contracts.

## Scope

1. Encode source ID and local page index in the existing 32-bit VM swap token
   as fixed by the WS contract; retain `UINT32_MAX` as the no-slot sentinel.
2. Convert boot `swapN` sources into manager entries with source ID N and
   preserve sparse boot numbering and numeric allocation order.
3. Add locked manager operations to prepare, claim, validate, publish,
   enumerate, mark draining, abort drain, and remove one source.
4. Reuse `ZEDSWAP1`/`ZEDSWAP2`, FAT extent, raw direct-I/O, duplicate identity,
   root-partition overlap, and ordered-flush validation.
5. Introduce a backing-object claim shared with write, truncate, rename,
   unlink, loop attachment, mount/disk writable aliases, and teardown. A
   second spelling or mount of the same inode/disk cannot bypass it.
6. Add a VM drain operation that prevents new target allocations, obtains
   stable ownership of each target-backed private page, pages it in, and
   repeats until target allocation and I/O counts are zero. Failure restores
   the source to active state without losing readable pages.
7. Add atomic VM commit-capacity updates. Removal is rejected before drain if
   existing commitment cannot fit in physical pages plus remaining swap.
8. Preserve resource and statistics accounting across every partial failure,
   retry, concurrent fault, and source-ID reuse.

## Non-goals

- exposing user control in this Phase;
- accepting more than four sources or changing swap-header formats;
- adding UFS-specific physical extent discovery;
- migrating a page directly from one swap source to another without making it
  resident; or
- changing the public boot-parameter contract.

## Verification

- Add production-linked host fixtures SWAP-T001--T006 covering encoded boundary
  slots, sparse boot IDs, add/remove/reuse, deterministic allocation,
  duplicate aliases, claim conflicts, partial preparation unwind, concurrent
  allocation/fault/drain, I/O retention, and shutdown.
- Exercise successful drain with target-backed pages, an `ENOMEM`/commit-limit
  rejection with no state loss, an injected page-in error, and retry after the
  error clears.
- Re-run BR-T45 and affected VM commit/reclaim/object/resource tests.
- Run `make -j16` and `git diff --check`; do not use `make check` or
  `.internal/`.

## Completion conditions

- every VM slot maps to the same source/local page for its complete lifetime;
- allocation cannot select a draining source;
- successful removal observes zero allocated and in-flight target slots before
  releasing any claim or backing resource;
- failed add or removal retains a coherent old state and accurate capacity;
- ordinary filesystem, loop, and disk aliases return `EBUSY` while claimed;
- current boot-selected source behavior and actual page-in/page-out remain
  unchanged; and
- SWAP-T001--T006 plus the affected regressions and build gate pass.

## Reconsideration boundary

Stop if safe drain requires holding VM metadata locks across backing I/O, if a
canonical disk/inode claim cannot cover separately mounted aliases, or if the
source-token layout conflicts with a live external ABI. Extract a separate VM
or VFS foundation Phase instead of narrowing `swapoff` correctness.

## Result (`q021`, 2026-08-28)

Implemented the four-source manager with stable source/local tokens,
PREPARED-to-ACTIVE publication, deterministic ID reuse, serialized lifecycle
transactions, live VM commitment resizing, and synchronous source drain. A
canonical backing-claim registry now protects FAT file extents and raw disk
ranges across inode, loop, mount, device, filesystem-write, and disk-teardown
paths. File/raw source preparation transfers claims only at manager
publication, and failed add/remove transactions restore the previous usable
state.

Verification evidence:

- `tests/run-phase001.sh`: BR-T45 and SWAP-T001--T006 PASS;
- manager, backing-claim, and VM-drain fixtures under ASan/UBSan: PASS;
- forced `make -B -j16`: PASS;
- `git diff --check`: PASS; and
- a disposable `build/amd64/hdd-image.img` booted through q35, xHCI, and USB
  storage to `login:` in 9.7 seconds. `loop0`, `loop1`, and `swap0` initialized,
  swap reported 16383 active slots, and no USB/loop/swap/VFS error appeared.

The common no-current-thread backing owner remains deliberately valid only
during the existing serialized early-boot interval. Runtime `bootN:` source
lifetime and enumeration metadata are owned by `ws016-p002`.

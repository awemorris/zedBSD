# WS003 Phase 015: four-platform boot-parameter acceptance

Last updated: 2026-08-27

WSID: `ws003`

Phase ID: `p015`

Combined ID: `ws003-p015`

Status: Completed (`q015`, 2026-08-27)

Parent: [WS003](../ws.md)

Contract: [kernel boot parameters](../../../docs/reference/kernel-boot-parameters.md)

Shared tests: [WS003 test index](../tests/README.md)

## Objective

Prove the completed parameter contract on i386 PC/AT, i386 PC-98, amd64 BIOS,
and amd64 UEFI, repairing integration defects until the declared matrix passes
or recording a genuinely platform-specific unresolved defect.

## Scope

- production images for all four target paths;
- reusable QEMU scripts and fresh disposable image copies;
- generated default overlay root and `swap0`;
- explicit `init=/bin/sh`;
- native `rootpart` without an overlay;
- file, raw-partition, and mixed swap sources;
- negative parameter combinations and visible failure diagnostics;
- device-order-independent UUID and PARTUUID selection regressions;
- native-root/raw-swap partition-alias rejection before either source is
  published; and
- final synchronization of WS003, WS013, docs, and test evidence.

## Non-goals

- physical Latitude repetition;
- UEFI Boot CPAR menu/FAT32-LFN implementation;
- legacy PC/AT or PC-98 CPAR menus;
- runtime swapon/swapoff; or
- broad unrelated platform repair.

## Acceptance matrix

Each row uses the platform's production loader and kernel, not a parser-only
test binary.

| Case | i386 PC/AT | i386 PC-98 | amd64 BIOS | amd64 UEFI |
| --- | --- | --- | --- | --- |
| Generated overlay default reaches normal init/login | required | required | required | required |
| `init=/bin/sh` reaches an interactive shell | required | required | required | required |
| `rootpart=` reaches a native non-overlay root | required | required | required | required |
| `swap0=boot0:swapfile` reports and exercises swap | required | required | required | required |
| Raw swap partition reports and exercises swap | required | required | required | required |
| Invalid root/overlay combination fails visibly | required | required | required | required |

Focused BR-T42--BR-T45 fixtures provide exhaustive multi-slot combinations.
The runtime matrix additionally includes, on both amd64 firmware paths:

- one mixed `swap0` file plus `swap1` partition case;
- one `boot1=UUID=...` cross-filesystem overlay-data case; and
- one `boot1=PARTUUID=...` cross-filesystem overlay-data case.

Both cross-filesystem cases attach the auxiliary disk before the production
boot disk, while firmware `bootindex` still selects the production image.  The
kernel must therefore resolve loader-origin `boot0` and the explicitly named
auxiliary `boot1` independently of `/dev/sd*` discovery order.  The complete
BR-T46 also includes one PC/AT negative cell that places a valid `ZEDSWAP2`
header in a disposable native UFS partition's reserved boot block and selects
that same partition through both `rootpart` and `swap0`.  The kernel must reject
the alias with `EEXIST` before swap activation, root mounting, or init.  The
complete matrix therefore contains 24 common cells, six amd64-only cells, and
one PC/AT-only alias cell, for 31 production-loader cells total.

## Work packages

1. Add BR-T46 scripts that build or consume the four production images and
   always operate on disposable copies.
2. Create deterministic native-root and signed raw-swap test layouts for each
   partition scheme, plus a distinct MBR/FAT auxiliary image with known FAT
   UUID and MBR PARTUUID for the amd64 device-reordering regressions.  Create
   the alias fixture by modifying only the UFS partition's reserved boot block;
   retain its superblock and filesystem contents.
3. Capture normalized parameter, root-mode, boot-slot, swap-source, and init
   markers without accepting a mere kernel-entry success.
4. Execute the matrix serially where tools or image files conflict and in
   parallel only where artifacts are independent.
5. Repair only defects within p011--p014 scope. Extract unrelated platform
   failures into a new Phase rather than silently expanding this one.
6. Run `make -j16`, all focused fixtures, applicable existing boot/storage/VM
   regressions, documentation link validation, and `git diff --check`.
7. Record exact commands, QEMU versions, image paths/hashes, logs, and results.

## Completion conditions

- every required matrix cell passes on a fresh disposable image;
- normal init, explicit shell init, native root, overlay root, FAT-file swap,
  and raw swap are all observed rather than inferred;
- every swap runtime cell reports positive page-out and page-in counters from
  an anonymous-memory pressure helper and verifies the restored page contents;
- invalid parameter sets fail at the named validation stage;
- both amd64 firmware paths boot with the auxiliary disk enumerated before the
  production boot disk, and resolve both UUID and PARTUUID `boot1` selectors
  without confusing them with loader-origin `boot0`;
- the PC/AT alias cell reports
  `vfs: validate rootpart swap alias failed (error 16)` and reaches neither
  swap publication, root mounting, nor init;
- no platform depends on the old `boot=`/`root=` or fixed image heuristics;
- the documentation matches implemented behavior and is promoted from
  `planned` to the appropriate implemented status; and
- WS003, WS013, master, and BR-T46 evidence agree on the result.

## q015 execution evidence

The authoritative fresh run is
`plan/ws003-bringup/temp/q015-br-t46-final-007`. It ran from
2026-08-27 11:04:12Z through 11:20:24Z and passed all 31 production-loader
cells: PC/AT 7/7, PC-98 6/6, amd64 BIOS 9/9, and amd64 UEFI 9/9. The system
QEMU version was 10.0.11 and the PC-98 build reported 11.0.93. Per-cell build
logs, image and parameter hashes, commands, guest/controller logs, and results
remain under that directory.

BR-T42--BR-T45 and the affected init, storage, VM, USB, and xHCI focused
regressions passed as recorded in the preceding Phase and shared-test evidence.

The generated/default init, explicit shell, native root, overlay root, file
swap, raw swap, and visible invalid-configuration cases passed on every
platform. Mixed file/raw swap, UUID reordering, and PARTUUID reordering passed
on both amd64 firmware paths. The PC/AT native-root/raw-swap alias cell emitted
the required pre-publication rejection and reached neither root nor init. All
10 positive swap cells reported nonzero page-in/page-out, full anonymous-page
readback, and `OBJECT-SHARED PASS`; no BOT/storage fatal marker was present.

The user's `config.mk` SHA-256 was unchanged before and after the run:
`3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
Authoritative evidence hashes are:

- `cells.tsv`:
  `d290ceb43b1f4b3c076c53b3b41d571dbfdf61b4526bd7821e03da5355efeb3c`;
- `results.tsv`:
  `d64dfbcc76d3c0f8e86f27ba86a7280391ca7a4e86fe0666c404c5d1f8a7d8cf`;
- `metadata.txt`:
  `e38233677fbebf399d89cd2688c9a98d65f6ec2bea1b81baedcb3f26a4fffd1b`.

`final-007` was run after the last USB/xHCI ownership review and therefore
supersedes `final-006`. The final source includes the reusable-URB ownership
barrier, xHCI submission/non-URB-operation stop gates, all-or-nothing DMA
release, and checked IRQ teardown. Their focused host regressions and one
post-correction runtime cell on each production platform passed before the
fresh 31-cell matrix.

## Dependencies

Depends on `ws003-p011`, `p012`, `p013`, and the p014 implementation/focused
gates. BR-T46's swap cells are deliberately shared as p014's runtime evidence;
both Phases close only after those cells prove actual page-out/page-in. It does
not require an additional physical-machine boot.

## Residual finding during BR-T46 development

The current `mmap` syscall recognizes `MAP_FIXED` when selecting its internal
path but rejects that flag in its public flag mask, leaving the replacement
path unreachable.  BR-T46 does not require replacement semantics and uses
`MAP_FIXED_NOREPLACE` for its exact-address object-backed pressure fixture.
The `MAP_FIXED` inconsistency remains a separately tracked implementation
defect; it is not hidden by weakening any BR-T46 acceptance condition.

## Reconsideration boundary

Mark only the affected matrix cell and Phase result uncleared if a QEMU model
cannot exercise a real production path or an unrelated platform defect blocks
the test. Do not weaken the parameter contract or call a parser fixture a boot
acceptance substitute.

# WS018 Phase 001: driver source-tree relocation

Last updated: 2026-08-28

WSID: `ws018`

Phase ID: `p001`

Combined ID: `ws018-p001`

Status: Complete (`q025`)

Parent: [WS018](../ws.md)

Tests: [WS018 test index](../tests/README.md)

## Objective

Move the repository-root `drivers/` source tree to the canonical
`src/drivers/` ownership boundary without changing driver behavior, public
driver interfaces, configuration choices, or produced image contents.  This
mechanical foundation lets later Phases reorganize input, graphics,
disk-label, and filesystem implementations without maintaining two driver
roots.

## Entry evidence

- Production driver sources and several source-private headers currently live
  in the repository-root `drivers/` directory; `src/drivers/` does not exist.
- `include/drivers/` already owns driver-facing headers and is a distinct
  public/include tree.
- All platform makefiles contain explicit source paths or object rules rooted
  at `drivers/`.  The X68k bootloader also compiles selected driver sources
  directly.
- Maintained WS test recipes and source audits contain direct driver source
  paths.  Historical closed Queue evidence may legitimately describe the old
  layout.

## Fixed design

- `src/drivers/` is the sole production driver source root after this Phase;
  the repository-root `drivers/` directory is removed.
- This Phase preserves filenames and relative layout wherever possible.  It
  does not perform the later semantic ownership changes for input, graphics,
  disk labels, or FAT.
- `include/drivers/` remains the driver interface header root.  Moving it or
  changing its include spelling is outside this Phase.
- Source-private headers currently stored beside a driver move with that
  driver.  A header is not promoted into `include/drivers/` merely to make a
  path move convenient.
- Build artifact paths such as `build/<platform>/drivers/*.o` may remain
  stable; source ownership and object-output naming are separate concerns.
- No kernel UAPI, HAL API, driver registration order, configuration symbol,
  or runtime behavior changes are authorized.
- Closed Queue books and historical result records are not rewritten.  Active
  build recipes, maintained tests, and normative documentation must use the
  new source path.

## Implementation procedure

1. Inventory every file below root `drivers/`, classify public versus private
   includes, and record every Makefile, script, test recipe, and direct source
   include that names the old path.
2. Relocate the complete tree to `src/drivers/` in one path-consistent change,
   preserving each driver's copyright, build flags, and private-header
   relationship.
3. Update all platform manifests and pattern rules, including amd64, PC/AT,
   PC-98, RPi4, sun4u, and X68k kernel and bootloader rules.  Do not silently
   drop a driver from a platform while repairing paths.
4. Update source includes only where a source-private header moved.  Preserve
   `<drivers/...>` for contracts that remain under `include/drivers/`.
5. Update maintained test compile recipes, dependency lists, and current
   source-layout documentation.  Do not consume `.internal/` or rewrite
   archived evidence.
6. Audit the dependency graph for remaining production inputs below root
   `drivers/`, then run the supported cross-platform build matrix.

## Verification

- `KA-T001`: a tree and dependency audit proves that root `drivers/` is absent
  and every production driver source resolves below `src/drivers/`.  Expected
  public includes under `include/drivers/` and object paths named `drivers/`
  are not false positives.
- `KA-T002`: the current supported manifests produce amd64, i386 PC/AT, i386
  PC-98, sun4u, RPi4, and X68k artifacts with the same configured driver sets.
- Compare linked object inventories before and after the move so path repair
  cannot accidentally omit an optional USB, storage, network, input, or
  graphics driver.
- Run `make -j16` for the configured build gate and `git diff --check`.  Use
  only maintained WS tests; do not run `make check` or use `.internal/`.

## Completion conditions

- No production source or source-private header remains under root
  `drivers/`, and no active build dependency reads from that path.
- `include/drivers/` and all stable driver/HAL/UAPI contracts are unchanged
  except for strictly necessary include dependency repair.
- Every supported platform still selects and links the same drivers.
- `KA-T001`, `KA-T002`, `make -j16`, and whitespace checks pass with recorded
  evidence.

## Execution record

Completed on 2026-08-28 in `q025`.

- Moved all 33 tracked files (26 C sources and seven source-private headers)
  from repository-root `drivers/` to `src/drivers/` without modifying their
  contents.  Every moved file's Git blob hash matched its pre-move blob.
- Repaired the amd64, i386 PC/AT, i386 PC-98, RPi4, sun4u, and X68k manifests.
  PC/AT and PC-98 retain their established `build/<platform>/drivers/*.o`
  paths and target-specific flags through an explicit source mapping; amd64
  retains its driver-specific compiler flags.  The X68k stage-2 direct source
  rule now reads the relocated private implementation and header.
- Updated the maintained WS003/WS004 direct-compilation recipes and current
  source-layout documentation.  Public `include/drivers/` paths and all
  driver/HAL/UAPI declarations were left unchanged.

`KA-T001` passed: root `drivers/` is absent, the active source/build audit has
no dependency on `drivers/*.c` or `drivers/*.h`, all 33 content hashes match,
and the six before/after manifest driver-name sets compare equal.  The
maintained DMA, PCI rescan, and PCIe capability host fixtures passed both
before and after relocation.

`KA-T002` passed with fresh, separate build trees:

| Target | Relocated driver objects | Result |
| --- | --- | --- |
| amd64 PC/AT | loop, DMA, PCI/PCI-PCAT, USB UHCI/EHCI/xHCI/storage, IDE, dp8390/NE2000, PS/2 mouse, graphics | kernel link and checker pass |
| i386 PC/AT | same configured set as amd64 | kernel link and checker pass |
| i386 PC-98 | loop, IDE, bus mouse, dp8390/LGY-98, GDC/glyph/Cirrus/auto graphics | kernel link and patch checker pass |
| RPi4 arm64 | loop, SDHCI | kernel link and checker pass |
| sun4u sparcv9 | loop, CMD646 | kernel link and checker pass |
| X68k m68k | loop, MB89352, SPC disk | kernel link, target audit, and stage-2 bootloader link pass |

The three unavailable system cross-compilers were unpacked only into the
ignored WS temporary directory; no host package was installed.  The normal
configured `make -j16` rebuilt the amd64 disk image successfully, and the
maintained SeaBIOS/q35/xHCI USB-only smoke reached `login:` from that image.
`git diff --check` passed.  Neither `make check` nor `.internal/` was used.

## Dependencies

This is the first WS018 source-layout Phase and has no WS018 predecessor.  The
later input, graphics, disk-label, core, and FAT Phases depend on this canonical
source root.

## Reconsideration boundary

Stop for human review if the move requires changing a public driver interface,
dropping a supported target or driver, merging implementations, changing
configuration semantics, or moving `include/drivers/`.  A build-system
limitation is recorded as `uncleared`; it is not justification for retaining
two production driver roots.

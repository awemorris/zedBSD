# WS018 Phase 006: boot implementation and public API consolidation

Last updated: 2026-08-28

WSID: `ws018`

Phase ID: `p006`

Combined ID: `ws018-p006`

Status: Complete (`q025`)

Parent: [WS018](../ws.md)

Tests: [WS018 test index](../tests/README.md)

## Objective

Make `src/kern/boot.c` the single implementation of architecture-independent
boot-parameter and boot-source policy, and make the existing
`include/kern/boot.h` the single, deliberately large public boot contract.
The consolidation must preserve every accepted name, value, error, lifetime,
handoff layout, and four-platform boot result.

This Phase treats the aggregate header as an interface ledger.  Once merged,
it is not reorganized for local implementation convenience; a later API
change requires a separately recorded substantive design decision.

## Preconditions and dependencies

- Complete `ws018-p001` so all manifests already use final source-tree paths.
- Complete `ws018-p005` so the dead image-loader API cannot be mistaken for a
  member of the boot aggregate.
- Preserve `ws018-p004`'s filesystem identity callback if that Phase has
  already run.  Source ordering does not change the boot-source resolver's
  dispatch behavior.
- Capture the q015/WS003 parser, boot-source, swap-source, loader-handoff, and
  31-cell runtime matrix results before moving declarations or definitions.

## Fixed public contract

The aggregate retains, without renumbering or semantic reinterpretation:

- the existing Stage 1/Stage 2 handoff structures, constants, enums, packed
  layouts, and static assertions already in `kern/boot.h`;
- `boot0`--`boot3`, `rootpart`, `overlay-root`, `overlay-data`,
  `swap0`--`swap3`, and `init` parameter keys and parse/query APIs;
- the self-contained parsed-parameter representation and its present-versus-
  empty-source distinction;
- boot-source selector/reference validation, failure stages, private-mount
  slots, retain/promote/publish/destroy ownership, and runtime lookup; and
- exact errno and cleanup behavior, including duplicate source rejection and
  immutable system-lifetime publication.

No alias header, compatibility macro, renamed enum, reordered ABI structure,
or newly public helper is introduced merely to simplify the merge.

## Work packages

### 1. Design the aggregate header before editing callers

Move the complete declarations and documentation from
`include/kern/boot-parameters.h` and `include/kern/boot-source.h` into
`include/kern/boot.h`.  Keep coherent sections in this order: shared loader
handoff ABI, architecture-independent parameter contract, boot-source
reference/ownership contract, then public functions.

Preserve all existing public identifiers, enum values, structure fields and
field order, constants, prototypes, and comments that specify errors or
lifetime.  Keep the existing handoff ABI section byte-for-byte equivalent.
Use forward declarations where legal and only the minimum includes required
for complete embedded types.  In particular, do not make the shared boot
header depend on implementation-private FAT/VFS state.

Because `kern/boot.h` is also included by bootloader and HAL code, add a
compile-only include fixture for every current consumer class: kernel, amd64
and i386 HAL, PC-98 handoff, and X68k Stage 2.  The header must remain
self-contained in all those freestanding contexts.

Update every source and maintained test to include `kern/boot.h` directly,
then delete the two smaller headers.  Do not leave forwarding headers: one
canonical public file is part of the accepted design.

### 2. Merge the three implementation units

Create `src/kern/boot.c` from, in dependency order:

1. parameter parsing and the parse-once global instance from
   `boot-parameters.c`;
2. pure selector/reference/root-mode/failure-name validation from
   `boot-source-contract.c`; and
3. private mount, lookup, retention, promotion, publication, cleanup, and
   runtime lookup from `boot-source.c`.

Keep implementation helpers `static`, audit same-name statics before merging,
and preserve the current public symbol set exactly.  Do not merge
`swap-boot.c`, VFS root selection, filesystem probing, loader-specific handoff
copying, or CPAR menu policy into this file.

Replace the three source/object entries with one `boot.c` entry in every
supported build and focused host-test recipe.  Delete the old source files only
after no source or manifest references them.

### 3. Freeze and audit the boundary

Generate a reviewed declaration/symbol inventory before and after the merge.
The post-merge inventory must contain the same boot handoff and public
parameter/source API, apart from the intentional header/source filenames.
Document in the header that declarations are appended or changed only after a
recorded architectural/API decision, not during ordinary refactoring.

## Verification

- KA-T050 compiles parser and source fixtures solely through
  `#include <kern/boot.h>` and covers syntax, duplicates, bounds, unknown keys,
  selector/reference validation, root-mode exclusivity, mount rollback,
  retain/promote/publish, and destruction.
- Header ABI fixtures verify the handoff structure sizes/offsets and parsed
  boot structure/enum values on the relevant 32- and 64-bit builds.
- WS003 parameter, source, swap-source, and handoff tests are updated to the
  aggregate include and retain their previous results.
- KA-T051 runs the established 31-cell matrix on i386 PC/AT, i386 PC-98,
  amd64 BIOS, and amd64 UEFI without a parameter, root, overlay, swap, or init
  regression.
- A tree audit finds only `src/kern/boot.c` for these implementations, only
  `include/kern/boot.h` for these public declarations, one definition of every
  API, and no old include/source/object reference.
- `make -j16` and `git diff --check` pass; `make check` and `.internal/` are not
  used.

## Completion conditions

- one public header exposes the unchanged handoff, parameter, and boot-source
  contracts and compiles in every existing consumer context;
- one implementation file owns the three former units without new public
  symbols or altered behavior;
- both smaller headers and all three former C files are deleted from source
  and manifests;
- the four-platform 31-cell matrix is green; and
- the stable-header policy is written into `kern/boot.h` beside the API.

## Reconsideration boundary

Stop for human review if aggregation requires changing a public signature,
enum value, structure layout, handoff include context, accepted parameter
grammar, or mount/source lifetime.  Do not solve include pressure by splitting
the header again, adding forwarding headers, exporting private VFS/FAT state,
or changing the bootloader ABI.

## Execution result

Completed on 2026-08-28 without reaching the reconsideration boundary.

- `include/kern/boot.h` now contains the unchanged loader handoff,
  architecture-independent parameter, and boot-source ownership contracts.
  It records the stable-header policy and remains self-contained without
  importing FAT, mount, filesystem, or either retired split boot header.
- `src/kern/boot.c` is the only architecture-independent implementation of
  parameter parsing, selector/reference/root-mode validation, private boot
  mount ownership, and runtime lookup.  The three old implementation units
  and two split headers were deleted with no forwarding shim.
- The pre/post symbol inventories contain the same 29 public definitions.
  KA-T050 passed freestanding 32/64-bit kernel, amd64/i386 HAL, PC-98 handoff,
  and X68k Stage 2 include contexts; handoff sizes/offsets, parameter enum
  values, and the 3136-byte parsed structure remained fixed.
- Existing BR-T42 parameter parsing, BR-T43 x86 handoff, BR-T44 boot source,
  and WS016 runtime boot-source lifetime tests passed through the aggregate
  header and source.
- Fresh links and architecture checkers passed for amd64, i386 PC/AT, i386
  PC-98, arm64/RPi4, sparcv9/sun4u, and m68k/X68k.  The normal configured
  `make -j16` and `git diff --check` passed.
- KA-T051 passed all 31 production-loader cells at
  `plan/ws018-kernel-architecture/temp/p006-br-t46-002`: PC/AT 7/7, PC-98
  6/6, amd64 BIOS 9/9, and amd64 UEFI 9/9.  `results.tsv` has SHA-256
  `9d16cd77854129f54156d68c0137aab0bdd98244ba64dc9acd3fd76ff3caac75`;
  the before/after `config.mk` SHA-256 remained
  `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.

The first matrix attempt exposed only stale generated dependency files in the
ordinary `build/{pcat,pc98,amd64}` trees that still named the pre-p001 root
`drivers/` sources.  Exactly those generated `.d` files were removed and
regenerated; no source or maintained input was removed.  The restarted matrix
then passed 31/31.

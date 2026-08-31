# WS001 Phase 019: canonical userland source headers

Last updated: 2026-08-31

WSID: `ws001`

Phase ID: `p019`

Combined ID: `ws001-p019`

Status: Complete (`agent2-q002`, 2026-08-31)

Parent: [WS001](../ws.md)

Style contract: [C coding style](../../coding-style.md)

Tests: [WS001 test index](../tests/README.md)

## Objective

Apply `plan/coding-style.md` section 13 to every C-family source file below
`userland`: each file starts at byte zero with the exact canonical zedBSD
copyright/SPDX block, followed by a separate non-empty file-explanation
comment. Preserve implementation behavior and useful existing prose.

## Audited baseline

The 2026-08-31 inventory contains 269 in-scope files:

- 214 `.c` implementation files;
- 44 `.h` headers; and
- 11 preprocessed `.S` assembler sources.

No file currently has both the exact canonical block and the required
separate explanation at byte zero. Most files use the former combined
one-line copyright/SPDX comment; nine use the canonical copyright block but
have no separate explanation; several retain useful project descriptions in
non-canonical leading comments; one C file has no license header; and one
starts with a UTF-8 BOM.

## Fixed scope and decisions

- `.c`, `.h`, and `.S` below `userland` are in scope. Makefiles, scripts,
  configuration/data, generated build output, vendored checkout contents, and
  binary/media assets are outside this C-style Phase.
- The canonical block is copied literally from section 13 and begins at byte
  zero. A blank line follows it. The next block is a multi-line comment with a
  concise role explanation, followed by another blank line.
- A modeline, when present, moves below those two required blocks. It cannot
  remain at byte zero because section 13 now says the copyright block starts
  the file.
- Preserve a useful existing description as the explanation, normalizing only
  its comment framing. Where none exists, write a file-specific role sentence;
  do not use one generic sentence for the whole tree.
- Preserve all bytes after the replaced leading-comment region, except removal
  of the one pre-header UTF-8 BOM. Do not reformat includes, declarations,
  functions, assembly, or unrelated comments.
- Do not change copyright ownership, year, or license identifier. The audited
  tree is locally owned and Zlib-licensed; any contrary ownership discovered
  during execution makes that file `uncleared` rather than overwritten.
- The completed `agent2-q001` implementation and the newly committed
  `plan/coding-style.md` are part of the fixed working baseline. The untracked
  Agent 2 Queue books and current planning changes must not be reverted,
  staged, or committed.

## Work packages

1. Add an exact whole-tree header audit and positive/negative fixtures. The
   audit rejects BOMs, missing/altered canonical fields, absent explanations,
   and a modeline or other byte before the required block.
2. Produce a deterministic inventory of every in-scope path and its selected
   explanation source: retained leading prose or a newly written role
   sentence.
3. Rewrite only each leading header region, retaining richer existing prose
   and placing any modeline after the explanation.
4. Review exceptional initial forms (`xlib`, X11 programs, runtime linker,
   Noct runtime, userland tests, `getconf-table.h`, and assembler sources)
   individually rather than forcing an unsafe textual substitution.
5. Run the whole-tree audit, fixture self-test, a body-preservation audit,
   focused preprocessing/compilation for changed header forms, the supported
   `make -j16` gate, and `git diff --check`.

## Verification contract

- The inventory count and audit-discovered count both equal 269; additions or
  removals cause a visible failure rather than silently changing the scope.
- Every file begins with the exact six-line canonical block and then a
  separate non-empty multi-line explanation.
- A fixture proves rejection of a BOM, modeline-first file, combined legacy
  header, altered owner/year/license, and missing explanation.
- A mechanical comparison proves that content following the migrated leading
  region is byte-identical for ordinary files; exceptional files have an
  explicit before/after review record.
- Every `.S` file still preprocesses for its owning architecture or is covered
  by the existing target build path; comments introduce no preprocessor or
  assembler tokens.
- `make -j16` and `git diff --check` pass. Aggregate `make check` is not used.

## Completion conditions

- all 269 audited C-family files pass the exact header/explanation audit;
- no ownership, year, SPDX value, implementation body, generated artifact, or
  unrelated user change is modified;
- reusable audit tooling and its fixtures are indexed under WS001;
- the final inventory and any truly exceptional handoff are synchronized into
  WS001 and `agent2-q002`; and
- focused checks, the configured build, and whitespace validation pass.

## Reconsideration boundary

Return a file or subtree as `uncleared` if it proves externally owned,
generated from a source outside this Phase, licensed differently, or unable to
accept C block comments at byte zero. Do not erase attribution, patch generated
output instead of its source, or extend this Phase into general C-style
normalization.

## Execution result

The final inventory is recorded in
[`migration-inventory.tsv`](migration-inventory.tsv): 214 `.c`, 44 `.h`, and
11 `.S` files, for 269 total. Seventeen explanations came from useful existing
prose and 252 are file-specific generated descriptions. The only BOM was
removed, and review found no contrary attribution or license.

Verification completed on 2026-08-31:

- `userland-file-header-audit.py` reports `T001` PASS for all 269 files and
  `T003` PASS for all 269 recorded implementation-body hashes;
- `userland-file-header-audit-test.sh` reports `T002` PASS for the positive and
  required negative fixtures;
- all 11 `.S` files pass `gcc -E -x assembler-with-cpp` with their repository
  include paths;
- the synchronized Phase 15 style audit and fixtures pass, retaining its
  separate 7-compliant/234-historical full-style classification;
- the configured `make -j16` compile/link/image build exits successfully; and
- `git diff --check` passes.

No implementation bytes, generated artifacts, ownership/license values, or
unrelated user files were changed. QEMU was intentionally omitted under the
fixed comment-only verification contract, and aggregate `make check` was not
used.

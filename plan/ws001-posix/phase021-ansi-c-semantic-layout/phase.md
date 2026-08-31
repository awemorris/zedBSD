# WS001 Phase 021: ANSI C declarations and semantic layout

Last updated: 2026-08-31

WSID: `ws001`

Phase ID: `p021`

Combined ID: `ws001-p021`

Status: Uncleared (`agent2-q005`, 2026-08-31)

Parent: [WS001](../ws.md)

Style contract: [C coding style](../../coding-style.md)

## Objective

Apply the clarified ANSI C and semantic-layout rules to all 214 C
implementations and 44 headers below `userland` without changing observable
behavior. Put the Emacs modeline above the copyright block, remove declarations
from `for` initializers and nested blocks, reject standalone scope-only blocks,
and expose each meaningful assignment/call paragraph and control-flow exit with
an English purpose comment and surrounding whitespace.

## Fixed interpretation

- ANSI C (C89/C90) is the language baseline. Reliably supported reserved-name
  extensions such as `__inline`, `__attribute__`, and `__asm__` remain allowed.
- Every automatic declaration belongs to the function-leading declaration
  group. Initializer evaluation stays at its original execution point.
- A standalone compound statement cannot be introduced or retained solely to
  scope declarations. If a name cannot safely move to function scope, the
  containing function must be split or the file remains uncleared.
- A semantic paragraph is a consecutive group of assignments and calls serving
  one purpose. Its comment normally begins with a present-tense English verb and
  names an object. Generated comments must not claim file-specific semantics
  that cannot be derived from the statements.
- Immediate operation/result checks form one paragraph. `else if` and `else`
  remain attached to their chain. Early guard returns use the decision comment;
  independent and final returns receive their own result/exit comment.
- A loop paragraph begins before any assignment that prepares the loop. The
  purpose comment introduces the preparation and traversal together, with no
  blank line between them.
- Every paragraph comment is separated from the preceding paragraph by a blank
  line, except at an opening brace or directly after a label.
- A meaningful callee result is stored or tested explicitly and is not returned
  as a compound expression. The caller exposes its success and failure paths
  without changing the callee's established return convention.
- The byte-zero modeline, blank line, copyright block, blank line, and file
  explanation order applies to all 269 C-family source files, including `.S`.

## Work packages

1. Extend the whole-tree audits and fixtures for header order, ANSI declaration
   placement, `for` declarations, standalone blocks, declaration/statement
   spacing, and semantic/control/return comments.
2. Move every nested declaration to its function declaration group, rename only
   where function-scope collisions require it, and replace safe initializers by
   assignments at their original execution points.
3. Rewrite every declaration-form `for` initializer and remove scope-only
   blocks without changing loop lifetime, `break`, `continue`, or `goto` paths.
4. Insert or refine purpose comments and blank lines for semantic paragraphs,
   decisions, loops, and returns. Split overly nested functions when declaration
   lifting would otherwise obscure ownership or lifetime.
5. Run idempotence checks, focused audit fixtures, the exact source-header gate,
   configured `make -j16`, and `git diff --check`.

## Completion conditions

- all 214 C files contain zero declaration-form `for` initializers, zero mixed
  or nested automatic declarations, and zero scope-only compound statements;
- every function has a leading declaration group separated from its first
  statement by one blank line;
- all 269 C-family files have the modeline/copyright/explanation order;
- semantic paragraphs, `if`, `for`, `while`, `switch`, and non-guard returns
  have adjacent purpose comments and required blank lines;
- all generated prose is grammatical and does not merely restate syntax;
- reusable audits and negative fixtures prove the mechanically decidable rules;
- `make -j16` and `git diff --check` pass; and
- any behavior-preservation obstacle is recorded by path and line rather than
  hidden behind a summary PASS.

## Reconsideration boundary

Do not change evaluation order, VLA size-evaluation timing, storage duration,
object representation, cleanup paths, externally visible diagnostics, or ABI
to satisfy presentation rules. Stop the affected file as `uncleared` if a
behavior-preserving function split or declaration lift cannot be established in
this Queue.

## Execution result

The mechanical part of this Phase is complete:

- the exact header audit passes for all 269 C-family files;
- all 214 implementations have zero declaration-form `for` initializers, zero
  declarations below the function-leading group, and zero actual standalone
  scope-only compound statements;
- 455 function declaration/statement gaps were normalized to one blank line;
- the ANSI/layout transformer is idempotent;
- the 214-C/44-header body audit reports zero mechanical residuals;
- the 2,454-function structural audit and both focused fixture suites pass;
- configured `make -j16` and `git diff --check` pass.

The Phase remains `uncleared` at work package 4. A strict adjacency inventory
finds 7,779 `if` decision sites without an immediately preceding one-line
purpose comment. Immediate operation/result checks can share a paragraph
comment, so each site needs semantic classification rather than blind comment
insertion. Assignment/call paragraphs and final or independent returns likewise
need per-function prose review. All 214 implementation rows remain
`semantic-review=uncleared` in `review-ledger.tsv`; no summary PASS masks this
residual.

Resume condition: approve a follow-up Queue that divides the 214 files into
reviewable semantic batches and requires file-specific V+O prose plus focused
build evidence for each batch.

### Agent 2 Queue 005 result

The clarified mechanical interpretation now applies across all 214
implementations:

- 374 loop-purpose comments precede their preparation assignments and the
  preparation remains adjacent to its loop;
- 7,727 previously uncommented decisions and 4,558 independent returns now
  have separated purpose/result paragraphs;
- 1,047 direct or compound call returns use typed result variables, including
  the atomic-width macro, while retaining short-circuit and return conversion
  behavior;
- the `at` main path has manually specific list/remove/input/time/open/submit
  prose and preserves the callee's zero-success return convention;
- the semantic transformer is idempotent and is enforced by the extended body
  audit and positive/negative fixture; and
- the zero-diagnostic body, structure, and header audits, fixtures, configured
  build, image regeneration, and whitespace gate pass.

The completion condition requiring human-reviewed, file-specific prose remains
uncleared. Common generated descriptions were refined to name bounds, parser,
file, process, result, and input objects and an erroneous `sizeof`/EOF heuristic
was eliminated, but deterministic prose is not evidence that every paragraph
states the best file-specific reason. All implementation ledger rows therefore
remain `semantic-review=uncleared`.

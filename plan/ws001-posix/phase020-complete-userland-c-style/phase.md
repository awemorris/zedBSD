# WS001 Phase 020: complete userland C-style conformance

Last updated: 2026-08-31

WSID: `ws001`

Phase ID: `p020`

Combined ID: `ws001-p020`

Status: Uncleared (`agent2-q003`, 2026-08-31)

Parent: [WS001](../ws.md)

Style contract: [C coding style](../../coding-style.md)

Tests: [WS001 test index](../tests/README.md)

## Objective

Apply every applicable rule in `plan/coding-style.md` to every `.c` and `.h`
file below `userland`. In particular, place all externally visible function
definitions before all static definitions, order the static implementation
top-down after that public surface, and give every non-`static inline` static
function a one-physical-line forward declaration in the declaration section.
Remove the former historical-file exception without changing behavior.

## Audited baseline

The 2026-08-31 tree contains:

- 214 `.c` implementation files and 70,355 lines;
- 44 `.h` files and 2,126 lines; and
- base, X11, and package-runtime subtrees totaling 72,481 C-family lines.

The Phase 15 inventory reports only 7 of the 241 base C/header files as having
its modeline and labels the other 234 historical. Its audit does not parse
functions and checks only the modeline, trailing whitespace, and same-line
`case` statements. Therefore its PASS output is not evidence for function
order, forward declarations, public/static comments, declaration placement,
loop/switch intent, call layout, allocation sequencing, returns, or semantic
paragraphs.

The 11 `.S` files are outside C declaration/function rules. They remain covered
by Phase 19's exact source-header and preprocessing evidence.

## Fixed interpretation

- A public function is a non-`static` function definition, whether or not the
  source spells `extern`. All such definitions precede every static function
  definition.
- The declaration section contains a one-line prototype for every normal
  static function. `static inline` definitions are the only exception named by
  the style contract.
- “Top-down” means that after the public definitions, static definitions are
  arranged in reader-facing call-flow order. Forward declarations, not
  bottom-up definition order, resolve calls to later helpers.
- Each public definition has the required multi-line verb-and-object comment;
  each static definition has a short one-line header comment.
- The exact Phase 19 copyright and explanation blocks stay at byte zero. The
  Emacs modeline follows those blocks and precedes includes.
- Header files apply the sections relevant to file organization, declarations,
  formatting, comments, macros, and whitespace. Rules requiring function
  definitions are non-applicable when no definition exists.
- Style-only edits preserve expression evaluation order, ownership, lifetime,
  diagnostic text, return values, external linkage, section placement required
  by the ABI, and observable calls. A required stylistic rewrite that cannot be
  shown behavior-preserving is recorded `uncleared`, not forced.

## Work packages

1. Replace the narrow Phase 15 checker with a whole-userland lexical/structural
   audit and positive/negative fixtures. It must inventory definitions,
   linkage, forward declarations, ordering, comments, loops/switches, split
   calls, local declarations, and other mechanically decidable rules without
   treating regex guesses as proof.
2. Create a deterministic 258-file review ledger. For every file, record each
   section 2--12/14 rule as `pass`, `not-applicable`, or `uncleared`, with
   mechanical diagnostics and manual-review notes where semantic judgment is
   required.
3. Normalize shared libraries and runtime support first, then base commands,
   shell/parsers/tests, X11, and package runtime. Compile and run focused tests
   after each cohesive batch.
4. For each `.c`, establish the required file sections, add all static forward
   declarations, move complete function blocks into public-before-static
   top-down order, and supply the required function/loop/switch comments.
5. Review every function body for declaration groups, debugger-friendly
   conditions and calls, braces around split controlled statements,
   allocation/check sequencing, comment form, semantic whitespace, final
   returns, and prohibited test-only environment controls. Make only
   behavior-preserving rewrites.
6. Normalize applicable header organization and declaration formatting, then
   run the whole-tree audit, manual-ledger completeness check, focused tests,
   Phase 19 regression, configured build, optional runtime gate under the Queue
   rule, and whitespace validation.

## Verification contract

- The inventory and audit both discover exactly 214 `.c` and 44 `.h` files.
- No implementation file contains a public definition after the first static
  definition; static definitions follow the reviewed top-down order.
- Every non-inline static definition has exactly one compatible one-line
  declaration in the forward-declaration section.
- Required public/static definition comments and loop/switch intent comments
  are present in the correct form and location.
- Every checklist item for every file is `pass` or justified
  `not-applicable`; no `historical`, `unknown`, or `uncleared` entry is hidden
  by a summary PASS.
- Compiler warnings or preprocessor failures introduced by declarations,
  linkage, movement, or comment placement are zero.
- Phase 19's 269-file exact-header audit still passes. Its original migration
  body hashes are a historical migration record and are not expected to match
  later authorized style refactoring.
- Applicable focused tests, `make -j16`, and `git diff --check` pass. Aggregate
  `make check` is not used.

## Completion conditions

- all 258 C/header files satisfy every applicable style rule;
- the user-identified public/static ordering and static-forward-declaration
  rules have executable whole-tree evidence;
- manual semantic review is recorded rather than claimed by an incomplete
  mechanical checker;
- public interfaces and observable behavior are unchanged;
- reusable audit and ledger checks are indexed under WS001; and
- actual results and any honest residuals are synchronized to the Phase,
  WS001, and `agent2-q003`.

## Reconsideration boundary

Stop a file as `uncleared` if applying a rule requires changing public ABI,
evaluation order, ownership/lifetime, externally visible diagnostics, generated
source, or architecture-required layout. Do not call an untouched historical
file compliant, weaken the style contract, or substitute build success for the
manual portions of the review checklist.

## Execution result

The complete 214-C/44-header structural pass is implemented. The reusable
audit reports 2,454 functions and zero structural diagnostics. It proves the
public-before-static rule, complete one-line static prototype set, split
definition headers, function comments, modelines, loop/switch comments,
same-line `case` removal, and trailing whitespace. Its positive/negative
fixture passes. The transformations are idempotent.

Safe local-declaration normalization was applied at function and nested-block
entry. It preserves initializer evaluation order and deliberately declines a
group containing a possible VLA. A compiler gate over configured userland
targets with `-Wdeclaration-after-statement -Werror` passes, as do the ordinary
configured `make -j16`, Phase 19's 269-file exact-header audit and fixture, and
`git diff --check`.

This Phase is nevertheless `uncleared`: the body audit currently reports 731
residuals (654 initialized automatic declarations outside the direct
`if`/`else` exception and 77 declarations in `for` initializers). All 86
malformed multi-line comment openings were normalized. In addition, sections
5, 6, 8, 9, 11, and 12
still require semantic per-function review. Rewriting array/aggregate/VLA
initialization or nested fallible calls without that review could change
evaluation order, lifetime, ownership, or diagnostics, which crosses the fixed
reconsideration boundary.

[`review-ledger.tsv`](review-ledger.tsv) contains exactly 258 source rows and
marks structural evidence separately from mechanical body residuals and the
still-uncleared semantic review. No `historical` classification is used.

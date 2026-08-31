C Coding Style
==============

## 1. Purpose

A C source is written for human readers and debuggers first.  Compact
expressions are not a goal.  A reader should be able to follow a
function from top to bottom, understand each processing phase from its
surrounding whitespace and comments, and stop a debugger on each
meaningful decision.

Declarations first, small guarded steps, explicit error exits, and
semantic paragraphs separated by blank lines.  It still contains
historical exceptions.  Those exceptions are not precedents; the rules
below are the canonical form for new and refactored code.

Style-only changes must preserve evaluation order, ownership,
lifetime, error reporting, and observable behavior.

## 2. File organization

C source files use the following basic order after the include directives:

1. file-wide macros and constants
2. enums
3. structures and other type definitions
4. file-scope variables
5. forward declarations
6. public function definitions
7. static function definitions

An implementation-local macro may remain next to the `static inline` functions
that use it when moving it to the top would make the implementation harder to
read.  This is a narrow exception for macros that are inseparable from the
local implementation.

Use the Emacs modeline used by the existing source files.  Indentation uses
tabs with a tab width of eight.

## 3. Forward declarations and function definitions

Every non-`static inline` static function has a forward declaration.  A forward
declaration is written on one physical line even when it is long:

```c
static bool parse_index(struct parse_ctx *ctx, const struct hir_expr *expr, int *offset);
```

Function definitions put the return type, function name, and every argument on
separate lines:

```c
static bool
parse_index(
	struct parse_ctx *ctx,
	const struct hir_expr *expr,
	int *offset)
{
	/* ... */
}
```

Public function definitions appear before static function definitions.  Put a
multiple-line comment before every public function definition.  Its first
sentence is a one-line verb-and-object description.  Additional explanation
may follow, but the comment does not cite design-document definition numbers.

```c
/*
 * Unrolls eligible scalar Packed loops.
 *
 * The transformed HIR remains valid for every backend.
 */
bool
hir_opt_unroll_func(
	struct hir_block *func_block)
```

A short one-line header comment is required for any static function.

## 4. Local declarations

Declare automatic variables before executable statements in their block.  Do
not initialize them in the declaration:

```c
int count;

count = 0;
```

An initialized declaration is allowed inside an `if` or `else` body when the
variable is local to that branch.  Keep a blank line after the declaration:

```c
if (has_value) {
	int value = get_value();

	use_value(value);
}
```

Put one blank line after the local declaration group.  Then write
`UNUSED_PARAMETER()` entries together, one per line and in argument order.  Put
assertions below them after another blank line:

```c
struct item *item;

UNUSED_PARAMETER(env);
UNUSED_PARAMETER(flags);

assert(source != NULL);
assert(result != NULL);
```

## 5. Semantic paragraphs and whitespace

Use blank lines to expose processing phases.  Allocation and its check,
initialization of one object, traversal of one collection, and publication of a
result are separate semantic paragraphs.

Do not put a blank line between an operation and an `if` that immediately checks
its result:

```c
item = hir_malloc(sizeof(*item));
if (item == NULL) {
	hir_out_of_memory();
	return NULL;
}

memset(item, 0, sizeof(*item));
item->type = HIR_ITEM_VALUE;
```

Place a blank line before an unrelated `if`, and between an independent `if`
and a following `if`-`else` chain.  Do not insert blank lines inside one
`if`-`else if`-`else` chain.

Place a blank line before the purpose comment that introduces a `switch`,
`for`, or `while` block unless the block immediately continues or checks the
preceding operation.  Keep the purpose comment adjacent to the block.

## 6. Debugger-friendly control flow

Prefer guard clauses and explicit decisions to compound Boolean returns.  Each
meaningful condition should provide a debugger stop point.

Do not write:

```c
return is_left(expr) || is_right(expr);
```

Write:

```c
if (is_left(expr))
	return true;
if (is_right(expr))
	return true;

return false;
```

Do not place several fallible or significant function calls in one condition.
Evaluate them in short-circuit order and check them one at a time:

```c
if (!check_expr(ctx, left))
	return false;
if (!check_expr(ctx, right))
	return false;

return true;
```

When a condition contains three or more clauses, or mixes nested `&&` and
`||`, put the clauses on separate lines and use parentheses and indentation to
expose its structure:

```c
if (expr == NULL ||
    (expr->type != HIR_EXPR_PLUS &&
     expr->type != HIR_EXPR_MINUS))
	return false;
```

A short condition with two side-effect-free clauses may remain on one line when
their relationship is immediately clear.  The rule against combining
significant calls still applies.

Preserve the original short-circuit evaluation order when decomposing a
condition.

## 7. Loops and switches

Every `for` and `while` loop has a one-line comment immediately before it that
states what the loop does.  The comment describes intent rather than restating
the syntax:

```c
/* Skip redundant parenthesized expressions. */
while (expr != NULL && expr->type == HIR_EXPR_PAR)
	expr = expr->val.unary.expr;
```

For a non-standard `for` loop, put each of its three control expressions on a
separate line:

```c
/* Append every remaining source statement. */
for (;
     source != NULL;
     source = source->next) {
	/* ... */
}
```

Put a one-line purpose comment immediately before every `switch`, just as for a
loop.  Separate the comment and switch from the preceding semantic paragraph
with a blank line.  A `case` label occupies its own line; never put a return,
assignment, or other statement on the same line as the label.

## 8. Function calls and control-statement bodies

Keep a simple call on one line.  If a call must be split, put the callee and
opening parenthesis on the first line and one argument on each following line:

```c
result = build_binary(
	HIR_EXPR_PLUS,
	left,
	right);
```

If the only statement controlled by `if`, `else`, `for`, or `while` spans
multiple physical lines, enclose that statement in braces:

```c
if (offset > 0) {
	return build_binary(
		HIR_EXPR_PLUS,
		counter,
		value);
}
```

Do not build deeply nested call expressions as arguments.  Store complex
intermediate results in named temporary variables, check each fallible result,
and then pass the completed values to the outer call.

## 9. Allocation and object initialization

Allocate and check one object at a time.  Do not perform several allocations
and then check them together:

```c
expr = hir_malloc(sizeof(*expr));
if (expr == NULL) {
	hir_out_of_memory();
	return NULL;
}

term = hir_malloc(sizeof(*term));
if (term == NULL) {
	hir_out_of_memory();
	return NULL;
}
```

Treat allocation plus its immediate check as one block.  Treat initialization
of one structure as another block.  Initialize one structure completely, then
separate it with a blank line before initializing another structure.

Check each fallible return value immediately and individually.  Do not execute
several fallible operations and test all their results afterward.

## 10. Comments

Use comments to expose why a processing phase or loop exists.  Do not narrate
obvious individual assignments.

Use this form for a comment that spans multiple lines:

```c
/*
 * The scalar cache cannot keep four division lanes live cheaply.
 * Keep this guard until the allocator exposes enough parallelism.
 */
```

Do not start a multi-line comment with prose on the opening `/*` line.

## 11. Returns

Normally put a blank line before the final return of a function.  The blank line
may be omitted when the function immediately returns the result just calculated
or assigned by the preceding statement.

Early guard returns stay adjacent to the condition they serve.

## 12. Test controls and environment variables

Do not put test-only environment-variable switches in production code.  The
implementation follows its default production behavior, selected by ordinary
configuration or compile-time architecture conditions.

An environment variable that implements an intentional diagnostic interface,
such as a supported debug trace, is not a test-only behavior switch and may
remain.

Tests must exercise the default production path.  They must not require a
hidden environment variable to enable or disable the implementation under
test.

## 13. Review checklist

Before finishing a C-source change, verify that:

- file sections and public/static function order are correct
- every normal static function has a one-line forward declaration
- every public function definition has a verb-and-object comment
- local declarations, and assertions follow the required order and spacing
- compound decisions and fallible calls are individually debuggable
- every loop and `switch` has an immediately preceding intent comment
- split calls use one argument per line and split controlled statements use
  braces
- allocations, checks, and per-object initialization form clear blocks
- final returns and semantic paragraphs have the expected blank lines
- no test-only environment switch controls production behavior
- the build, focused tests, and `git diff --check` pass

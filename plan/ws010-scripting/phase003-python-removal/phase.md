# ws010-p003: x86 dependency-closure audit

WSID: `ws010`

Phase ID: `p003`

Status: complete

Parent WS: [WS010](../ws.md)

## Objective

Audit the complete direct and transitive dependency closure of the three
required disk-image and boot-acceptance paths, and remove Python from those
paths. Unreached Python elsewhere in the repository is explicitly out of scope.

## Work packages

- [x] Record the Make dry-run Python inventory for all three x86 targets.
- [x] Include runtime imports and subprocess-invoked checker dependencies.
- [x] Replace all reachable Makefile and script Python invocations with Noct.
- [x] Retain old `.py` files only for targets outside the frozen production closure.
- [x] Keep `.internal/` wholly outside the build and test dependency graphs.

## Completion record

Forced dry-runs of all three production `disk-image` targets contain neither a
Python command nor a `.py` path. The frozen 15-script closure has Noct
replacements, and none invokes Python. Repository Python outside this closure
remains available to out-of-scope platform and specialist targets.

## Completion conditions

- No command in any of the three dry-run or actual disk-image builds invokes Python.
- No migrated Noct script invokes Python or reads `.internal/`.
- Each replacement passes a focused valid-input and invalid-input contract case.
- Remaining Python files are proven unreachable and listed as out of WS010 scope.

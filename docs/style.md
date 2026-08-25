# Documentation rules

Status: current

## Document classes

- `docs/architecture/` explains why components are shaped as they are.
- `docs/reference/` specifies exact observable interfaces and compatibility.
- `docs/howto/` gives reproducible procedures for a user goal.
- `plan/` owns proposals, Phases, status, acceptance, and future work.

Do not use a plan as the only product reference for behavior that users can
rely on. Conversely, do not present a proposed interface in product
documentation as implemented.

## Required status and evidence

Every product document starts with one status: `current`, `experimental`,
`deprecated`, or `planned`. Interface references name supported architectures,
source/header locations, applicable tests, known limitations, and stability.
Architecture documents visibly separate current behavior from intended design.
Commands and procedures include an expected observation and meaningful failure
diagnostics.

Compatibility terms such as POSIX, SUS, Linux-compatible, and FreeBSD-compatible
must identify the covered version/profile and known deviations. A behavioral
similarity is not called binary compatibility unless ABI layout is also tested.

## Links and navigation

Use relative Markdown links for repository documents. Every document is
reachable from [the documentation index](README.md), a section index, or the
authoritative [plan index](../plan/README.md). Renames update inbound links in
the same change. Run the repeatable link validator documented by
[WS009 tests](../plan/ws009-documentation/tests/README.md).

Documentation rules
===================

Status: current documentation rules

These rules follow the project owner's policy (2026-10-03).

## Documents come first

`docs/` holds the design of zedBSD and Keiland: what each part is for, how it
is divided, and which interfaces programs and people can rely on. A document
states the target design. Implementation follows the document, the way code
follows its tests in test-driven development. A document may therefore
describe something that is not built yet. That is not an error, and the
document does not need to mark it.

When the design changes, change the document first and then the code. A
document that disagrees with the intended design is wrong. A document that is
ahead of the code is not.

## Document classes

- `docs/architecture/`: the external design of a subsystem. It covers its
  purpose, its parts and their boundaries, and the reasons for them. Internal
  implementation is left to the source.
- `docs/reference/`: exact interfaces that programs and people use: commands,
  configuration, file formats, user APIs and compatibility.
- `docs/howto/`: procedures for a user's goal, each with the observation that
  shows it worked.

## No links into plan/

`plan/` is a separate world. It holds work in progress (Queues, Phases,
status and acceptance), and its records are deleted when the work is done. A
document in `docs/` never links into `plan/` and does not depend on anything
there. Schedules, open decisions and the state of the work belong in `plan/`,
not in `docs/`.

Links to the source tree (headers, programs, configuration) are fine.

## Writing

- Begin each document with a status line that says what kind of document it
  is (for example `design`).
- Use relative Markdown links within the repository. Every document is
  reachable from [the documentation index](README.md) or a section index.
  When a document is renamed, update the links to it in the same change.
- Compatibility terms such as POSIX, Linux-compatible and FreeBSD-compatible
  name the version or profile they mean. Do not call a similarity "binary
  compatibility" unless the ABI is the same.

# zedBSD documentation

Status: current navigation index

This tree contains product documentation: descriptions of implemented zedBSD
behavior, public interfaces, and reproducible operator/developer procedures.
Project scheduling and unfinished design work belong in the [planning
tree](../plan/README.md).

## Information architecture

- [Architecture](architecture/README.md): subsystem boundaries and design
  explanations.
- [Reference](reference/README.md): exact commands, configuration, UAPI, and
  compatibility contracts.
- [How-to guides](howto/README.md): goal-oriented build, boot, and
  administration procedures.
- [Documentation rules](style.md): status, evidence, links, and writing rules.

Empty sections are intentional landing pages. A producer Phase adds a document
only after it can distinguish current behavior from planned behavior and cite
the relevant implementation and evidence.

## Status vocabulary

- `current` documents installed, accepted behavior.
- `experimental` documents implemented behavior whose public interface may
  still change.
- `deprecated` documents behavior that still exists but is scheduled for
  removal; removal is never implied before it occurs.
- `planned` documents design or future work and makes no implementation claim.

A planned document may also identify an explicit manual hold. That hold means
work cannot enter an execution Queue until the named user decision releases it;
it is not an implementation failure.

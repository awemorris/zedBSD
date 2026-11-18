# Tests

Scenario tests for zedBSD and Keiland: what a person does with the system and
what they should see, written so that an agent can carry them out on a real
machine or in QEMU (the Agent Acceptance Test). How they are organised,
written, chosen and run is in [plan/tests.md](../plan/tests.md).

- `scenarios/os/` — operating-system functions
- `scenarios/desktop/` — the desktop
- `scenarios/apps/` — applications
- `suites/` — named sets of scenarios (`smoke`, `full`, per area and per application)

Each scenario, in Markdown or JSON, states its purpose, the operations, what to
check, the correct result and how to check it. Scenarios are source: change
them together with the behaviour they describe.

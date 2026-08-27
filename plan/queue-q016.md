# Queue: checked legacy PCI USB IRQ teardown

Last updated: 2026-08-27

QID: `q016`

Queue status: complete

Queue finished: **Yes**

Authorization: explicitly approved by the user on 2026-08-27 as the first
automatic Queue in the ordered WS execution run

Timebox: continuous execution through 2026-08-28 09:00 JST; this finite Queue
contains one Phase and may finish earlier

Parent: [master plan](master.md)

Previous Queue: [q015](queue-q015.md)

## Purpose

Complete the highest-priority dependency-ready Phase, `ws004-p009`, before
moving automatically to the next WS. Convert the legacy EHCI and UHCI detach
paths to checked PCI IRQ removal so an in-flight INTx handler can never retain
a pointer to released controller, IRQ, HCD, BAR, or I/O state.

This Queue does not change the supported-device policy for MSI-less xHCI.
xHCI is already on the checked teardown path; this Queue repairs the distinct
EHCI/UHCI INTx lifetime defect.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p009` | [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase009-pci-hcd-irq-teardown/phase.md), [tests](ws004-hardware/tests/README.md) | complete | EHCI and UHCI retain all ownership on checked IRQ removal failure, retry safely, pass focused lifecycle tests, and build for i386 PC/AT and PC-98 |

## Entry evidence and dependencies

- q015 completed the checked PCI IRQ API and migrated xHCI.
- EHCI and UHCI still call the legacy void wrapper and free resources even when
  the checked implementation retained a busy IRQ cookie.
- The repair is bounded to two PCI HCD detach paths and focused fixtures; no
  unresolved public UAPI or product decision remains.
- The user's active priority order places WS004 before WS012, WS008, WS016,
  WS017, and WS001.

## Ordered execution

1. Audit attach, running, unregister, detach, and retry ownership for EHCI and
   UHCI, including controller interrupt masking and active USB work.
2. Convert both drivers to checked IRQ removal and return the failure without
   freeing any handler-reachable or retry-required state.
3. Ensure successful retry completes exactly once and leaves no stale driver,
   bus, IRQ, BAR/I/O, DMA, or controller registration.
4. Add focused host lifecycle fixtures for initial `EBUSY`, later success,
   persistent failure, and double-release prevention on both HCDs.
5. Run the focused tests, affected PCI/USB regressions, i386 PC/AT and PC-98
   production builds with `make -j16`, and `git diff --check`.
6. Record results in the Phase, WS004, master, and this Queue; archive the
   finished Queue as `queue-q016.md`.
7. If the Phase completes or is honestly deferred/uncleared, construct the
   next dependency-ready Queue automatically according to the approved WS
   priority order.

## Stop, defer, and continuation rules

- A routine implementation defect within EHCI/UHCI/PCI/USB lifetime ordering
  remains in scope and is repaired without asking for a product decision.
- If safe repair truly requires a new common PCI detach state machine or a
  public contract decision, record the exact issue in p009/WS004, mark p009
  `uncleared`, report the required judgment in the conversation, and continue
  with the next independent WS.
- No physical test is required. Do not use `make check`, `.internal/`, or a
  repository commit. Preserve unrelated working-tree changes.

## Approval boundary

The user explicitly approved automatic Queue construction and execution on
2026-08-27, with no shorter time limit and a 2026-08-28 09:00 JST outer bound.
This Queue authorizes only `ws004-p009`; later work receives a new numbered
Queue record before implementation.

## Result

`ws004-p009` completed without a human-decision blocker. EHCI and UHCI now use
the USB HCD checked-quiesce boundary to mask and halt the controller, disable
PCI bus mastering, and remove INTx through the checked PCI API before any DMA,
IRQ allocation, PCI window, list node, or controller is released. Failed
quiesce or removal retains complete ownership for retry; staged attach cleanup
quarantines the controller if safe rollback itself cannot complete.

The HW-T02 lifecycle fixture, strict driver syntax builds, i386 PC/AT image
build, forced full i386 PC-98 image build, config-preservation check, and
`git diff --check` all pass. The pre-existing PC-98 full-build diagnostic
`/bin/sh: 1: -u: not found` remained non-fatal; the image checker completed
with `BIOS HDD image check: PASS`. No `make check`, `.internal/`, physical
test, or commit was used.

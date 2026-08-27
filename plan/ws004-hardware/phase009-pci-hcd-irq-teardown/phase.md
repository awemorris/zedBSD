# WS004 Phase 009: checked PCI HCD IRQ teardown

Last updated: 2026-08-27

Phase ID: `ws004-p009`

Status: planned; not in the active Queue

Parent: [WS004 hardware expansion](../ws.md)

## Objective

Remove the remaining interrupt-handler lifetime race from the legacy PCI USB
host-controller detach paths. An HCD must not free its IRQ allocation,
controller object, BAR mapping, or handler argument until checked interrupt
disestablishment has succeeded.

## Finding carried from q015

q015 added `drv_pci_device_disestablish_irq_checked()` for xHCI. The checked
path masks INTx/MSI/MSI-X first, returns `EBUSY` while a handler is in flight,
and retains its cookie and mappings on every failure. xHCI now retries that
contract and retains all DMA on failure.

EHCI and UHCI still call the legacy void wrapper and then continue teardown.
If i386 reports an in-flight INTx handler, the cookie is retained but those
drivers can still free the IRQ allocation and controller used as the handler
argument. That is a potential use-after-free and is outside q015's xHCI boot-
parameter scope.

## Planned work

1. Convert EHCI and UHCI detach to the checked PCI IRQ API.
2. Mask controller interrupt generation before checked disestablishment.
3. On `EBUSY` or another error, retain the controller, IRQ allocation, BAR/I/O
   ownership, USB bus, and every handler argument for a safe retry.
4. Order USB HCD quiesce/unregister, IRQ removal, bus-master disable, BAR
   release, and controller free explicitly; do not leave a half-detached bus.
5. Add host fixtures for first-attempt `EBUSY`, later success, persistent
   failure, and proof that no resource is released early.

## Completion conditions

- EHCI and UHCI use checked IRQ teardown and propagate failure;
- every failed removal retains all state needed by an in-flight handler;
- a retry can finish exactly once without double-free or stale registration;
- focused host lifecycle tests pass on both controller paths;
- i386 PC/AT and PC-98 production builds pass with `make -j16`; and
- `git diff --check` passes without `make check` or `.internal/` use.

## Reconsideration boundary

Stop for a separate common-PCI design decision if safe conversion requires a
new generic detach state machine shared by non-USB PCI drivers. Do not hide a
busy handler by reverting to unchecked cookie or controller release.

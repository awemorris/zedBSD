# WS004 Phase 009: checked PCI HCD IRQ teardown

Last updated: 2026-08-27

Phase ID: `ws004-p009`

Status: Complete (`q016`)

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

## Scope clarification: i386, MSI, and xHCI

This Phase is not a workaround for an MSI-less xHCI controller. The xHCI
driver already prefers MSI-X, then MSI, and finally INTx, and its checked IRQ
teardown was completed in q015. A platform policy may separately decline to
attach xHCI when neither MSI-X nor MSI is available; that is a supported-device
decision, not the repair described here. Also, i386 does not inherently imply
that PCI MSI is unavailable: availability depends on the platform interrupt
backend and the device capabilities.

The remaining race belongs to EHCI and UHCI. Those legacy HCDs deliberately
allocate INTx and must remain safe when an INTx handler is in flight. Disabling
xHCI on an MSI-less i386 target would therefore leave the EHCI/UHCI lifetime
bug unchanged. `ws004-p009` remains the bounded repair for those two drivers.

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

## Implementation and evidence

The existing `drv_usb_hcd_ops.quiesce` contract was sufficient; no new common
PCI state machine or human decision was required. EHCI and UHCI now:

- preflight USB-bus ownership through `drv_usb_hcd_unregister()` before making
  the controller inert;
- mask controller interrupts, halt scheduling with a bounded wait, disable and
  read back PCI bus mastering, and boundedly retry
  `drv_pci_device_disestablish_irq_checked()` from `quiesce`;
- free schedule/frame-list DMA from `stop` only after checked quiesce succeeds;
- retain the HCD registration, IRQ cookie/allocation, DMA, BAR or I/O claim,
  saved PCI command state, controller, and handler argument on every failure;
- restore the saved PCI enable state and release each resource once on a
  successful retry; and
- unlink a detached controller from the root-probe list, eliminating the stale
  list-node use-after-free in both legacy HCDs.

The staged attach cleanup also closes the directly related BAR-claim, IRQ-
allocation, PCI-enable-state, and ignored-unregister failure leaks. An attach
whose safe cleanup cannot finish keeps the PCI driver bound and quarantines the
complete controller instead of freeing handler-reachable state.

Evidence completed on 2026-08-27:

- `pci-hcd-irq-teardown-test.c` passes initial `EBUSY` followed by success,
  persistent `EBUSY`, persistent `EIO`, retained ownership, and exactly-once
  final release for both HCD models;
- both changed drivers pass `cc -std=c11 -Iinclude -Wall -Wextra -Werror
  -fsyntax-only`;
- i386 PC/AT production image generation passes with `make -j16` and the
  q015 Phase-owned PC/AT configuration;
- i386 PC-98 passes a forced full `make -B -j16` production image rebuild and
  `BIOS HDD image check: PASS` with the matching Phase-owned configuration;
- `config.mk` remains byte-identical at SHA-256
  `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`;
  and
- `git diff --check` passes. Neither `make check` nor `.internal/` was used.

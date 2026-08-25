# ws004-p003: ECAM and MSI interrupt ownership

Last updated: 2026-08-25

WSID: `ws004`

Phase ID: `p003`

Combined ID: `ws004-p003`

Status: complete software milestone; cross-target link evidence carried forward

Parent WS: [WS004](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Add validated ACPI MCFG/ECAM discovery and the dynamic amd64 APIC-vector
ownership required for PCI MSI/MSI-X. Preserve one architecture-neutral HAL
contract that can later resolve a PCI source through arm64 IORT/GIC ITS without
exposing an ITS object, DeviceID, LPI, or architecture-specific message layout
to the PCI layer.

## Human decision record

The following API is fixed for this Phase:

```c
int hal_irq_register_msi(
    const char *source,
    hal_irq_handler_t handler,
    void *handler_arg,
    int *mapped_irq,
    paddr_t *mapped_addr,
    uint32_t *mapped_event);

int hal_irq_unregister_msi(int mapped_irq);
```

`paddr_t` will be introduced as an alias of the existing `hal_physaddr_t`.
This does not create a second physical-address representation or change its
ABI.

The decision selects a unified logical IRQ namespace. A successful
registration allocates the logical IRQ and architecture interrupt-controller
resource, installs the handler, and returns the exact address/event payload
which the bus layer must program. No opaque public MSI handle is added.

## API contract

### Source namespace

The initial accepted source is exactly:

```text
PCI SSSS:BB:DD.F
```

- `SSSS`, `BB`, and `DD` are fixed-width lowercase hexadecimal fields.
- Segment is 16-bit, bus is 8-bit, device is `00` through `1f`, and function is
  `0` through `7`.
- Leading/trailing whitespace, aliases, shortened fields, and trailing text are
  rejected.
- PCI core creates this identifier from `struct drv_pci_address`; drivers never
  construct it.
- amd64 validates but does not otherwise need the BDF. A future arm64 ITS
  implementation resolves the segment and requester ID through firmware
  internally.
- USB devices are not an MSI source family: a PCI xHCI function uses its PCI
  identifier. Future non-PCI message sources require a separately specified
  canonical prefix and are not guessed in this Phase.

The string is required only for the duration of the call. HAL does not retain
the caller's pointer.

### Outputs and failure atomicity

- `mapped_irq` is a unique kernel logical IRQ accepted by
  `hal_irq_unregister_msi()` and passed to the registered handler.
- `mapped_addr` is the physical address to program into the device MSI/MSI-X
  capability.
- `mapped_event` is the device message-data value: an APIC vector on amd64 and a
  future ITS EventID on arm64.
- `source`, `handler`, and all output pointers must be non-null. A malformed
  source is rejected before allocation.
- Outputs are written only after complete success. Every partial allocation is
  rolled back before returning an error.
- Repeated registration for the same source is allowed and allocates one vector
  per call until the pool is exhausted.
- Failures use existing HAL status values; no new errno namespace is introduced.

### Lifecycle and concurrency

- One call registers one interrupt vector. Initial conventional MSI support is
  exactly one vector; MSI-X entries may be registered individually.
- PCI setup order is: keep device interrupt generation disabled, register with
  HAL, validate and program the returned message, enable the device, and on
  teardown mask and drain the device before unregistering the HAL mapping.
- Unregister rejects unknown, legacy, already removed, and currently executing
  self-unregister requests. It prevents new handler entry, waits for an
  in-flight handler on another CPU, then removes the logical IRQ and vector
  mapping.
- HAL masking gates kernel dispatch only; it cannot stop MSI writes from the
  device. Device-side masking or interrupt disable remains a PCI-layer duty.
- Dynamic affinity changes for an MSI mapping are unsupported in this Phase.
  Registrations target CPU 0. Changing the target would require returning a
  recomposed address/event pair, which this API intentionally does not do.

### PCI capability width and rollback

- HAL is unaware of the selected PCI capability's address and data widths.
- MSI-X accepts a 64-bit address and 32-bit event. Conventional MSI accepts the
  address width advertised by its capability and a 16-bit event.
- PCI validates the returned values before programming the device. If either
  value does not fit, PCI unregisters the mapping and returns a stable error.
- The initial amd64 fixed-edge message values fit both capability forms. The
  width checks remain mandatory so a future architecture fails atomically.

## Implementation work packages

### P003.1: Public HAL contract and all-port boundary

- Add `paddr_t` as the canonical alias of `hal_physaddr_t` and declare the two
  fixed APIs in the public HAL IRQ header.
- Document ownership, output validity, one-vector semantics, handler calling
  convention, and error cases beside the declaration.
- Add explicit `HAL_ERR_UNSUPPORTED` implementations to ports without MSI
  support so every configured architecture continues to compile and link.
- Retain the current `hal_irq_handler_t` acknowledgement contract; MSI handlers
  acknowledge through the architecture's normal EOI path.

### P003.2: ACPI MCFG and ECAM

- Generalize checked ACPI root-table lookup beyond MADT and add MCFG discovery.
- Validate table length/checksum, allocation-entry alignment, segment/range
  ordering, bus bounds, base alignment, and physical-address overflow.
- Add 4 KiB ECAM configuration access while keeping PCI mechanism 1 as the
  explicit fallback when no valid segment-0 ECAM window covers the request.
- Scan only firmware-declared bus ranges and preserve segment identity in PCI
  addresses and the canonical HAL source string.

### P003.3: amd64 logical IRQ and vector allocator

- Separate legacy GSI identity from logical IRQ identity; remove the assumption
  that every IRQ is in the current 1-through-15 ISA range.
- Reserve exception, syscall, legacy IRQ, notify, TLB, error, and spurious IDT
  vectors before constructing the dynamic MSI pool.
- Dispatch the full dynamic vector pool through an explicit vector-to-logical-
  IRQ mapping rather than arithmetic on `INT_IRQ_BASE`.
- Reuse the common handler, in-flight tracking, acknowledgement, and teardown
  rules for legacy and MSI logical IRQs.
- Compose an amd64 fixed-delivery, edge-triggered CPU-0 MSI address/event and
  guarantee rollback on every allocation or IDT installation failure.

### P003.4: PCI MSI/MSI-X integration

- Parse MSI and MSI-X capability chains with loop, bounds, BAR, table-size, and
  mapping validation.
- Build the canonical source string centrally from `struct drv_pci_address`.
- Extend the PC/AT PCI bus allocator and generic establish/free paths to support
  one-vector MSI and individual MSI-X entries while preserving INTx fallback.
- Store the logical IRQ and returned message in the PCI interrupt cookie so
  device drivers remain insulated from message layout.
- Implement device masking, width validation, enable ordering, and complete
  rollback for setup, attach failure, and detach.

### P003.5: Evidence and integration

- Add table-driven source-parser and MCFG validation fixtures under the WS test
  directory.
- Cover vector exhaustion, reserved-vector exclusion, reuse after unregister,
  duplicate unregister, in-flight teardown, and self-unregister rejection.
- Cover conventional MSI and MSI-X register images, capability-width rejection,
  masking order, rollback, and repeated registrations for one source.
- Boot QEMU with `q35` and a deterministic MSI-capable PCIe test device; prove
  handler delivery, EOI, detach, and continued login-shell operation.
- Build all supported architecture configurations to prove unsupported-port
  stubs preserve the global HAL ABI.

## Affected areas

| Area | Planned effect |
|---|---|
| HAL types and IRQ headers | `paddr_t` alias and fixed register/unregister declarations |
| amd64 IDT/APIC/IRQ | Dynamic vector pool, logical mapping, dispatch, EOI, teardown |
| Non-amd64 HAL ports | Explicit unsupported implementations |
| amd64 ACPI | Checked generic lookup and MCFG parsing |
| PC/AT PCI | ECAM access and MSI/MSI-X bus operations |
| Generic PCI | Capability parsing, source construction, cookies, rollback |
| First MSI consumer | Exercise through an existing xHCI or NVMe PCI function without a driver-specific HAL API |
| WS004 tests | Parser, allocator, register-image, lifecycle, and QEMU evidence |

## Completion criteria

- The exact decided API is present and all configured ports compile and link.
- Valid MCFG windows provide ECAM access; invalid or absent MCFG has a tested,
  bounded fallback.
- amd64 can allocate, dispatch, acknowledge, unregister, and reuse a dynamic MSI
  logical IRQ without colliding with reserved vectors.
- PCI can establish and tear down at least one-vector conventional MSI and one
  MSI-X entry with complete failure rollback and INTx fallback.
- A real QEMU PCIe device delivers an MSI through the new path and remains
  detachable without stale dispatch or loss of console/login operation.
- The API remains sufficient for a future arm64 ITS implementation even though
  IORT/ITS support itself is not part of this Phase.
- Deferred multi-message MSI and dynamic MSI affinity are recorded in the WS
  follow-up register rather than silently treated as complete.

## Reconsideration triggers

Return to human judgment instead of extending the signature implicitly if any
of these occur:

- A supported PCI source cannot be resolved from segment plus BDF on a target
  architecture.
- A target requires an event wider than 32 bits or an address wider than
  `paddr_t`.
- Safe affinity migration requires a public operation that returns a new
  address/event pair.
- The first QEMU hardware fixture requires an external implementation or a
  driver-visible architecture-specific API.
- One-vector registration cannot support the first selected xHCI or NVMe
  integration path.

## Result

Completed on 2026-08-25 in `q004` without changing the decided API.

- The public HAL contract and `paddr_t` alias are implemented. amd64 validates
  canonical PCI sources, owns a 16-vector dynamic MSI pool, and provides
  concurrency-safe register/unregister behavior. Other HAL ports provide an
  explicit unsupported boundary.
- ACPI MCFG parsing validates checksums, lengths, entry alignment, bus-range
  overlap, and address bounds. amd64 maps selected ECAM windows into a dedicated
  permanent MMIO window; PC/AT PCI retains bounded mechanism-1 fallback.
- Generic PCI establishes and tears down one-vector MSI and MSI-X mappings with
  device-side disable/mask ordering, width checks, rollback, and INTx fallback.
- Focused source, MCFG, PCI capability/rescan, and MSI/MSI-X register-image
  binaries pass with `-Wall -Wextra -Werror`. `make -j16` and the amd64 image
  checker pass.
- QEMU `q35` with the EDU device reports `WS004 MSI ALLOCATOR PASS vectors=16`,
  `WS004 MSI DELIVERY PASS status=1`, `init: system running`, and `login:`.
  This proves real message delivery through ECAM configuration and continued
  system operation.
- amd64, PC/AT i386, and PC-98 kernels link. arm64, sparcv9, m68k, and i386
  unsupported stubs pass strict syntax checks. Their full target links remain
  an environment handoff because the corresponding cross compilers are not
  installed; this does not require an API reconsideration.

Deferred multi-message MSI, dynamic affinity, arm64 IORT/GIC ITS, and non-PCI
source families remain in the WS follow-up register.

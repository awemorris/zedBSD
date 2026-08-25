# WS009 Phase 001: documentation information architecture

Last updated: 2026-08-25

Phase ID: `ws009-p001`

Status: complete

Parent: [WS009](../ws.md)

Tests: [WS009 test index](../tests/README.md)

## Objective

Create the stable product-document hierarchy and make repository-relative
Markdown links repeatably verifiable without restoring retired test material.

## Scope and decisions

- `docs/architecture/` explains subsystem design and boundaries.
- `docs/reference/` defines observable interfaces and compatibility.
- `docs/howto/` provides reproducible task procedures.
- `plan/` remains the source for future work, status, and acceptance records.
- Product documents declare current, experimental, deprecated, or planned
  status and never merge intended behavior with implemented behavior.

Moving historical documents merely to satisfy the new hierarchy is out of
scope; producer Phases add or migrate them when their accuracy can be checked.

## Work packages

- [x] Inventory the current product-document tree.
- [x] Create the product navigation and section landing pages.
- [x] Publish status, evidence, compatibility, and navigation rules.
- [x] Add a recursive Noct relative-link validator under WS009 tests.
- [x] Remove one stale link to a retired repository-level test CSV.
- [x] Validate every relative file link under `docs/` and `plan/`.

## Acceptance and result

DOC-T00 passes:

```text
Markdown relative-link check: PASS (246 links at CW closure)
```

The validator intentionally does not fetch web links or validate fragment
anchors. Those are separate future checks; this Phase's gate is repository
relative-file integrity and active navigation.

## Resume point

Extract DOC-20 for a clean build-from-source guide, or let the next producer
UAPI Phase jointly extract its matching WS009 reference item.

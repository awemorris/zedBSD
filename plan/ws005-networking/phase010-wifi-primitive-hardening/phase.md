# WS005 Phase 010: primitive WLAN abnormal-path hardening

Last updated: 2026-09-01

WSID: `ws005`

Phase ID: `p010`

Combined ID: `ws005-p010`

Status: planned; deliberately deferred until p009 proves basic communication

Parent: [WS005 networking and WLAN](../ws.md)

Tests: [WS005 test index](../tests/README.md)

## Objective

After the minimum direct command and physical IP path work, complete the
primitive command's abnormal and semi-normal behavior without mixing that
work into initial bring-up.

## Scope

- exhaustive public-command arity, boundary, escaping, and output-ceiling
  cases;
- scan generation replacement, empty/in-progress/failed snapshots, truncation,
  and count/size overflow;
- connection rejection stages, timeout, signal, cancellation, concurrent
  disconnect, device detach, and stale-result handling;
- repeated idempotent start/stop/disconnect and cleanup on every exit;
- complete userspace secret-erasure and diagnostic-redaction matrix; and
- ordinary, sanitizer, analyzer, and bounded stress variants for the primitive
  command and its production ioctl dispatcher.

Private `--machine`, fd-4 secret transport, ZNV2 parsing, `networkd` child
lifecycle, and orchestration failures remain p006/p007. Rekey, automatic
reconnect, firmware recovery, USB hotplug/reset, and hardware lifetime remain
WS004 p030. p010 tests their public command-facing error propagation but does
not reimplement those owners.

## Dependencies

- p004 minimum direct command complete;
- p009 one-run physical communication checkpoint complete; and
- any machine interface required by p006 remains owned and tested with p006.

## Completion conditions

- Every declared public CLI/input/output boundary has deterministic coverage.
- All terminal, cancellation, race, and detach paths retire command-owned
  storage and return a truthful bounded result.
- No success or failure evidence leaks credential or derived key material.
- Focused ordinary/sanitizer/analyzer/stress gates and affected build/boot
  regressions pass.

p010 is required before final p008 acceptance, but is not a prerequisite for
proving the first useful communication path in q059.

# WS005 Phase 010: primitive WLAN abnormal-path hardening

Last updated: 2026-09-05

WSID: `ws005`

Phase ID: `p010`

Combined ID: `ws005-p010`

Status: planned; p009 proved basic communication and the user explicitly
prioritized this hardening after the 5-GHz normal path; Queue-ready after
WS004 p044

Parent: [WS005 networking and WLAN](../ws.md)

Tests: [WS005 test index](../tests/README.md)

## Objective

After the minimum direct command and physical IP path work, make `/sbin/wifi`
the sole owner of the finite high-level connect retry and complete the
primitive command's abnormal and semi-normal behavior without mixing that work
into initial bring-up.

## Scope

- exhaustive public-command arity, boundary, escaping, and output-ceiling
  cases;
- scan generation replacement, empty/in-progress/failed snapshots, truncation,
  and count/size overflow;
- one monotonic 30-second `wifi connect` deadline which starts/observes
  asynchronous scan and single-attempt connect generations, retries only
  within that command, and never asks an ioctl to wait for the whole deadline;
- connection rejection stages, retryable/fatal classification, timeout,
  signal, cancellation, concurrent disconnect, device detach, and stale-result
  handling;
- repeated idempotent start/stop/disconnect and cleanup on every exit;
- complete userspace secret-erasure and diagnostic-redaction matrix; and
- ordinary, sanitizer, analyzer, and bounded stress variants for the primitive
  command and its production ioctl dispatcher.

Private `--machine`, fd-4 secret transport, ZNV2 parsing, `networkd` child
lifecycle, and orchestration failures remain p006/p007. Protocol-local frame
retransmission, rekey, firmware recovery, USB hotplug/reset, and hardware
lifetime remain WS004 p030. WS004 p044 removes p030's high-level automatic
reconnect and supplies prompt asynchronous primitives plus link events.
Resident daemon policy is p011. P010 does not retain a process after success
and does not implement post-success monitoring.

The 2026-09-05 public `net wifi` amendment does not change this primitive
grammar: `/sbin/wifi` remains interface-specific for root recovery and for
networkd's private child. It does not discover all `wlanN`, read `wifi.conf`,
own the active policy UID, or retain a passphrase. Global enable/disable/list/
connect/disconnect and all-interface arbitration belong to p007/p011.

## Dependencies

- p004 minimum direct command complete;
- p009 one-run physical communication checkpoint complete; and
- WS004 p044 asynchronous primitive ownership complete; and
- any machine interface required by p006 remains owned and tested with p006.

## Completion conditions

- Every declared public CLI/input/output boundary has deterministic coverage.
- One invocation performs all of its scan/select/connect retries inside one
  userspace 30-second deadline; kernel generations remain asynchronous and no
  hidden kernel reconnect occurs after the command exits.
- All terminal, cancellation, race, and detach paths retire command-owned
  storage and return a truthful bounded result.
- No success or failure evidence leaks credential or derived key material.
- Focused ordinary/sanitizer/analyzer/stress gates and affected build/boot
  regressions pass.

p010 is required before final p008 acceptance, but is not a prerequisite for
proving the first useful communication path in q059.

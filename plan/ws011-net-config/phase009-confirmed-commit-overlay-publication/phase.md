# WS011 Phase 009: confirmed-commit overlay publication correction

Last updated: 2026-09-05

WSID: `ws011`

Phase ID: `p009`

Combined ID: `ws011-p009`

Status: Queue-ready; proposed as q075

Parent: [WS011](../ws.md)

Uncleared acceptance: [p007](../phase007-confirmed-commit-acceptance/phase.md)

Evidence: [q074 QEMU evidence](../tests/q074-confirmed-commit-qemu-evidence.md)

Tests: [WS011 test index](../tests/README.md)

Queue proposal: [q075](../../queue.md)

## Objective

Locate and correct the target-runtime stop in atomic `/etc/net.conf`
publication on the hybrid root overlay, then rerun only the uncleared
NCOM-T021 confirmation/reboot cell and return p007 to its completion gate.

## Trigger and known boundary

Q074's NCOM-T020 timeout/client-loss cell passed. Its one NCOM-T021 cell
successfully armed the transaction, preserved the old startup view, and
completed the confirmed check plus full candidate reconcile. Ten admitted
networkd operations appeared after ordinary `commit`, but the client did not
return `Commit complete.` within 30 seconds. No DISARM request or fatal
diagnostic appeared.

The production order bounds the stop to `netconf_save_atomic_locked()` after
runtime reconcile and before confirmed DISARM. That interval is validate,
same-directory temporary open/write/`fflush`/file `fsync`, close, overlay
rename replacement, and backing synchronization. Q074 cannot distinguish
those stages and its two-cell allowance is exhausted.

## Scope

- one phase-owned target helper or temporary diagnostic markers which identify
  the last completed atomic-publication stage without changing public grammar
  or protocol;
- one fresh diagnostic amd64/PC-AT hybrid-overlay QEMU cell, only if static and
  host evidence cannot establish the stage;
- a bounded correction in the netconf writer, generic file-sync path, overlay
  replacement path, or its directly responsible backing filesystem path;
- focused fault/order tests proving prior-file preservation, successful atomic
  replacement, returned errors, no false completion, and confirmed DISARM only
  after durable publication;
- one fresh NCOM-T021-only acceptance cell after the deterministic correction
  gates pass, followed by the p007 non-QEMU gates and planning synchronization.

## Non-goals

- rerunning the already accepted NCOM-T020 cell;
- physical acceptance p008, a remote-shell service, VLAN/bridge p004, or any
  public command/protocol change;
- weakening `fsync`, atomic rename, runtime-before-persistence, or
  persistence-before-DISARM ordering;
- accepting an arbitrarily longer timeout without evidence that publication is
  making bounded forward progress;
- a broad VFS or storage redesign.

## Ordered work packages

- [ ] NCOM-P01: Add deterministic stage observations and reproduce the exact
      temporary-write/flush/close/replace sequence against an overlay whose
      destination is lower-only. Include injected failures and a finite host
      deadline before using QEMU.
- [ ] NCOM-P02: If the host fixture cannot identify the target-only stage, run
      at most one instrumented disposable QEMU cell. Record the last completed
      stage, elapsed bounds, backing topology, and absence or presence of timer
      expiry; do not treat this diagnostic cell as acceptance.
- [ ] NCOM-P03: Implement only the responsible local correction. Preserve
      durable atomic replacement, the old valid file on pre-rename failure,
      exact error propagation, and the confirmed transaction's armed state
      until publication succeeds.
- [ ] NCOM-P04: Pass the new stage/order/failure fixture plus NCOM-T001--T012,
      parser, console, persistence, boot, ZNV2, managed-WLAN/Wi-Fi, and
      maintained amd64/i386 `net`/networkd builds. Do not run aggregate
      `make check`.
- [ ] NCOM-P05: Run exactly one fresh NCOM-T021-only QEMU acceptance cell with
      the q074 topology and real one-minute timer. Require new file bytes,
      post-deadline stability, reboot persistence, connectivity, and unchanged
      production-input digests.
- [ ] NCOM-P06: If T021 passes, mark p007 complete and leave p008 gated only by
      its physical choices. Otherwise leave p007/p009 uncleared with the newly
      narrowed stage and extract any materially broader correction separately.

## Completion conditions

- The exact blocking stage and responsible lock, wait, or persistence operation
  are demonstrated by deterministic evidence rather than inferred solely from
  a controller timeout.
- The corrected atomic writer returns success or a bounded exact error; it
  never reports completion before durable publication and never disarms a
  rollback transaction after an uncertain or failed publication.
- Lower-only destination replacement on the production hybrid overlay has a
  focused success and failure regression.
- One fresh NCOM-T021-only cell proves atomic new persistence, no late rollback,
  reboot restoration, and usable synthetic networking.
- P007's selected regressions/builds/docs checks pass and all production inputs
  remain digest-identical.

## Interruption boundary

Stop and return `uncleared` before weakening durability, adding a public ABI,
running a second diagnostic or acceptance retry, changing unrelated VFS/storage
semantics, or undertaking a material filesystem redesign. Do not repeat the
q074 ordinary-commit attempt without new stage evidence.

## Current result and resumption

The failure boundary and corrective procedure are extracted from q074. No
production correction or additional QEMU execution is authorized until q075
receives explicit approval.

# WS011 Phase 009: confirmed-commit overlay publication correction

Last updated: 2026-09-05

WSID: `ws011`

Phase ID: `p009`

Combined ID: `ws011-p009`

Status: completed (`q075`, 2026-09-05); causal FAT correction and four post-fix cells pass

Parent: [WS011](../ws.md)

Completed acceptance: [p007](../phase007-confirmed-commit-acceptance/phase.md)

Evidence: [q074 QEMU evidence](../tests/q074-confirmed-commit-qemu-evidence.md)

Corrective evidence: [q075 evidence](../tests/q075-confirmed-commit-evidence.md)

Tests: [WS011 test index](../tests/README.md)

Queue result: [q075](../../queue-q075.md)

## Objective

Locate and correct the target-runtime stop in atomic `/etc/net.conf`
publication on the hybrid root overlay, then rerun only the uncleared
NCOM-T021 confirmation/reboot cell and return p007 to its completion gate.

## Trigger and known boundary

Q074's NCOM-T020 timeout/client-loss cell passed. Its one NCOM-T021 cell
successfully armed the transaction and preserved the old in-memory startup
view. Ten admitted networkd connections appeared after ordinary `commit`, but the client did not
return `Commit complete.` within 30 seconds. No DISARM request or fatal
diagnostic appeared.

Authentication is logged before request decoding/dispatch. The tenth request
is expected to be final DNS, but its completion is not established. The
unobserved interval includes DNS handling/response/client close, subsequent
`netconf_save_atomic_locked()` (validate, temporary open/write/flush/sync/close,
rename and backing synchronization), and DISARM connection setup. Q074 cannot
distinguish these stages and its two-cell allowance is exhausted. The cell
was terminated before the one-minute rollback deadline, so absence of expiry
is not evidence that its timer failed.

## Scope

- one phase-owned target helper or temporary diagnostic markers which identify
  final request completion and the last completed atomic-publication stage without changing public grammar
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

User amendment (2026-09-05): after the one instrumented cell did not reproduce
the stop, the user authorized ten trials with slightly varied procedures and
explicitly allowed clearance if none reproduces it. This supersedes the
single post-fix acceptance allowance and exact-cause completion requirement;
it does not establish a causal repair when no defect is localized.

Subsequent amendment: normal case 06 did reproduce the stop and the user
explicitly approved thorough optimization of that identified path. The cause
is excessive FAT-chain traversal during the startup writer's flush. Keep case
06 as a pre-fix failure; use remaining cases 07--10 for corrective acceptance,
with 07 replaying the failing procedure. Do not claim ten non-reproductions.
Optimize the mount-locked validation/seek/write traversal, retaining complete
corruption checks and durability; no persistent validation cache is introduced.

## Amended ten-cell verification matrix

All cells use fresh disposable disks, normal optimized binaries with tracing
and extra debug/frame-pointer flags disabled, the same old/candidate address,
one-minute confirmed timer, 30-second command bound, 70-second post-confirmation
wait, and reboot/persistence/route/DNS/gateway checks. Each case changes only
the listed feature from the original sequence.

| Cell | Variant | Changed procedure |
| --- | --- | --- |
| 01 | baseline | Original sequence; 15 ms host key spacing |
| 02 | baseline | Slower input: 30 ms key spacing |
| 03 | baseline | Faster input: 5 ms key spacing |
| 04 | confirm-delay-1 | Wait one second immediately before ordinary confirmation |
| 05 | confirm-delay-5 | Wait five seconds immediately before ordinary confirmation |
| 06 | show-candidate | Display the candidate before confirmation |
| 07 | show-candidate | Post-fix replay of the procedure that failed in case 06; replaces the unconsumed extra startup display |
| 08 | edit-roundtrip | Edit candidate ne0 to .18, then back to .17 before arming |
| 09 | upper-startup | First ordinary-commit unchanged old intent, making startup upper-backed |
| 10 | repeat-commit | Ordinary-commit once more after confirmation, before leaving net |

Do not count the earlier instrumented run toward these ten. On recurrence,
retain the failed image, monitor CPU/register/stack and memory snapshots for
diagnosis; no blind retries or speculative durability changes.

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

- [x] NCOM-P01: Add deterministic stage observations and reproduce the exact
      temporary-write/flush/close/replace sequence against an overlay whose
      destination is lower-only. Include injected failures and a finite host
      deadline before using QEMU.
- [x] NCOM-P02: If the host fixture cannot identify the target-only stage, run
      at most one instrumented disposable QEMU cell. Record the last completed
      stage, elapsed bounds, backing topology, and absence or presence of timer
      expiry; do not treat this diagnostic cell as acceptance.
- [x] NCOM-P03: Implement only the responsible local correction. Preserve
      durable atomic replacement, the old valid file on pre-rename failure,
      exact error propagation, and the confirmed transaction's armed state
      until publication succeeds.
- [x] NCOM-P04: Pass the new stage/order/failure fixture plus NCOM-T001--T012,
      parser, console, persistence, boot, ZNV2, managed-WLAN/Wi-Fi, and
      maintained amd64/i386 `net`/networkd builds. Do not run aggregate
      `make check`.
- [x] NCOM-P05: Under the user amendments, process ten fresh NCOM-T021-only
      QEMU cells with the q074 topology and real one-minute timer. Preserve the
      case 06 failure and require post-fix cases 07--10 to pass with new file bytes,
      post-deadline stability, reboot persistence, connectivity, and unchanged
      production-input digests.
- [x] NCOM-P06: If T021 passes, mark p007 complete and leave p008 gated only by
      its physical choices. Otherwise leave p007/p009 uncleared with the newly
      narrowed stage and extract any materially broader correction separately.

## Completion conditions

- The exact blocking stage and responsible lock, wait, or persistence operation
  are demonstrated by deterministic evidence rather than inferred solely from
  a controller timeout, OR all ten varied normal-build trials pass and the
  result is explicitly cleared as non-reproduction under the user's amendment.
- The corrected atomic writer returns success or a bounded exact error; it
  never reports completion before durable publication and never disarms a
  rollback transaction after an uncertain or failed publication.
- Lower-only destination replacement on the production hybrid overlay has a
  focused success and failure regression.
- All ten authorized cells are accounted for. Following the reproduced case 06
  and causal correction, all four remaining normal-build cells, including its
  procedural replay, prove atomic new persistence, no late rollback, reboot
  restoration, and usable synthetic networking. Pre-fix failure remains in the
  record; this is not clearance by ten non-reproductions.
- P007's selected regressions/builds/docs checks pass and all production inputs
  remain digest-identical.

## Interruption boundary

Stop and return `uncleared` before weakening durability, adding a public ABI,
running beyond the amended ten normal cells, changing unrelated VFS/storage
semantics, or undertaking a material filesystem redesign. Do not repeat the
q074 ordinary-commit attempt without new stage evidence.

## Current result and resumption

The diagnostic and normal cases 01--05 passed guest observations. Case 01 had
a separately repaired host predicate error, revalidated without another boot.
Normal case 06 timed out in the startup writer's flush. Live/core state shows
net running in FAT validation with an unlocked buffer-cache lock; the preserved
32 MiB DATA.IMG chain is healthy and acyclic. Floyd's distant cursors defeat
the mount's one-sector cache, and each data sector also repeats the initial
seek. The correction uses single-forward Brent validation to capture the
initial position and old tail, then reuses an operation-local cursor across
data sectors and zero-fill/growth. Full validation still precedes every write,
including corruption beyond the requested range. No persistent trust or
durability/rollback relaxation was introduced.

The exact final 32 MiB fixture reduces 4 KiB writes near the start/end from
16,399/17,423 backend reads to 138/138. Old HEAD fails the enforced cost gate;
the final cost suite passes 1,660,787 checks in each ordinary/sanitizer mode.
The independent cursor suite passes 237,441 checks per mode, including 55
growth write-failure positions, and the maintained FAT suite passes 441,782
checks per mode. Writer/overlay and all selected WS011/ZNV2/Wi-Fi regressions,
amd64/i386 builds, and documentation gates pass.

Cases 07--10 all pass on the same normal source image, including the exact
case 06 procedural replay. Confirmation returns in 1,072--1,173 ms of host
observation including input/polling; all four cases retain new startup bytes
after 70 seconds and reboot, with usable synthetic networking and unchanged
within-cell inputs. The pre/post-fix net and networkd ELF files are byte-identical;
instrumentation is not the causal repair. Case 06 remains a pre-fix failure,
not ten successful non-reproductions. P007 is complete. No implementation work
remains here; p008 awaits its physical choices and p004 remains manually held.

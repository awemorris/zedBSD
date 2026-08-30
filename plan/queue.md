# Queue: Archer identity intake and network authorization foundation

Last updated: 2026-08-31

QID: `q040`

Queue status: in-progress

Queue finished: **No**

Authorization: after q039's automatic repairs completed, the user explicitly
requested continuous execution of the remaining workstreams. The standing
priority order selects the first dependency-ready WLAN evidence checkpoint and
then an independent control-plane prerequisite rather than waiting on physical
label evidence.

Timebox: none. Process both finite items to `completed` or `uncleared`. A human
label checkpoint in the first item does not block the second item.

Parent: [master plan](master.md)

Previous Queue: [q039](queue-q039.md)

## Purpose

Capture every safely obtainable identity, descriptor, provenance, and license
fact for the exact connected Archer T3U Nano without binding a zedBSD driver.
Then implement the independently specified AF_UNIX peer-credential and network
authorization foundation required before ordinary users or desktop software
may request WLAN orchestration through `networkd`.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p026` | [Phase](ws004-hardware/phase026-archer-t3u-nano-identity-firmware/phase.md) | uncleared | Exact descriptor, independent ID mappings, firmware/license pin, package boundary, base absence, and negative inputs are recorded; only the non-inferable printed model/region/revision awaits one external observation |
| 2 | `ws005-p003` | [Phase](ws005-networking/phase003-unix-peer-credentials/phase.md) | in-progress | Add immutable AF_UNIX `SO_PEERCRED`, transactional `root:network 0660` publication, peer-based `networkd` authorization, and central root checks for direct mutating network ioctls with focused and runtime evidence |

## Dependency and deferral decisions

- `ws004-p027` is not selected in q040. It depends on a completed p026 target
  capability/firmware boundary; a USB descriptor cannot substitute for the
  printed hardware revision required by p026.
- `ws005-p003` depends on the already frozen `ws005-p002` topology, not on the
  physical WLAN adapter or p027. It proceeds even if p026 is uncleared.
- Intel Mac `ws020-p004` remains an external physical checkpoint and its final
  repetition remains after the uncleared p003 matrix. It does not block the
  next software workstream.
- PC-9821V13 `ws003-p023` retains its one exact diagnostic-image observation
  from q039 and does not block q040.

## Fixed boundaries

- The p026 remote-host action is one read-only descriptor and package
  inventory. Do not bind, reset, upload firmware to, or exercise the radio.
- Redact serial identifiers. Do not infer a printed hardware revision from a
  product string, USB `bcdDevice`, shared vendor archive, or VID:PID alone.
- Do not commit a Realtek blob to the zlib-licensed base tree and do not add a
  build-time or runtime network download.
- p003 publishes one fixed 12-byte zedBSD peer-credential ABI and connection-
  time snapshot semantics. Do not substitute live PID lookup or payload-supplied
  identity.
- Keep one `root:network 0660` `/run/networkd.sock`; do not add setuid clients,
  a world-writable socket, or a second WLAN daemon socket.
- Reject direct network mutation for non-root in the kernel before driver or
  route state changes. Preserve query paths and root recovery commands.
- Do not consume `.internal/` or run aggregate `make check`. Use `make -j16`,
  focused owner tests, disposable QEMU images, and `git diff --check`.
- Commit after each Phase. Retain a local commit if push is unavailable.

## Completion definition

q040 is finished when both selected items have been processed to `completed`
or `uncleared`, with exact evidence and resume conditions synchronized into
their Phase, WS, and master books. A missing printed adapter revision is an
allowed p026 uncleared result; it is not permission to widen a USB match or to
skip the independent p003 security work.

# Queue: managed WLAN completion

Last updated: 2026-09-05

QID: `q071`

Queue status: in-progress

Queue finished: **No**

Authorization: after resolving the kernel/userspace/networkd responsibility
split, the user explicitly requested implementation.  The request also accepts
the minimal read-only BSD-style `AF_ROUTE`/`RTM_IFINFO` notification described
by `ws004-p044`; no general route mutation interface is authorized. On
2026-09-05 the user froze the global public WLAN grammar, one-connection
selection order, authenticated policy-owner model, daemon-side credential-store
read, and no-retained-passphrase boundary for p006/p007/p011.

Parent: [master plan](master.md)

Previous Queue: [q070](queue-q070.md)

## Purpose

Reach a useful managed WLAN implementation without interleaving repeated human
hardware gates: first add the RTL8822BU Japan W52 normal path, then establish
the asynchronous kernel boundary, make `/sbin/wifi` the sole owner of the
30-second connect sequence, complete the private daemon protocol and six global
`net wifi` forms, and finally let `networkd` own the active policy UID and
four persistent global states without retaining credentials. RF loss gets one
same-selected-SSID 30-second userspace attempt before clean auto-searching
fallback.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | `ws004-p041` | in-progress | RTL8822BU W52 5-GHz normal path; existing 2.4-GHz baseline |
| 2 | `ws004-p044` | in-progress; focused gates pass | Prompt asynchronous scan/connect, no kernel high-level reconnect, minimal interface event stream; p041 |
| 3 | `ws005-p010` | in-progress; focused gates pass | Sole userspace 30-second retry and primitive abnormal paths; p044 |
| 4 | `ws005-p006` | in-progress; focused gates pass | Versioned global no-secret ZNV2 requests, authenticated policy UID, fixed-store reader, and fd-4 child transport; p010 |
| 5 | `ws005-p007` | in-progress; focused gates pass | Implement the six interface-free `net wifi` forms, stable all-WLAN selection, and one managed connection; p006 |
| 6 | `ws005-p011` | in-progress; focused gates pass | Own four persistent states plus bounded transients and one same-SSID 30-second recovery child, with no retained passphrase; p007 |

## Execution policy

- Process every dependency-ready item and synchronize actual evidence into its
  P/W/M books.
- A newly discovered human decision marks only the affected item `uncleared`;
  continue with any independent ready item.
- Use focused fixtures, configured amd64/i386 `make -j16`, and bounded QEMU or
  the already-authorized `10.0.10.25` USB-passthrough checkpoint.  Never run
  aggregate `make check`.
- Keep credentials in volatile test input or approved `.internal/` material;
  never copy them into plans, commits, logs, or retained evidence.
- Accept only `net wifi set-key SSID PASSPHRASE [auto]`, `enable`, `disable`,
  `list`, `connect SSID`, and `disconnect`; omission of `auto` means manual,
  the explicit literal `manual` is rejected, and no public command has an
  interface operand.
- Keep `net` stateless. Before any `enable` mutation, networkd validates the
  authenticated peer UID's fixed store and completes a stable radio
  enumeration. It installs or switches the active policy UID only after that
  preparation succeeds, and never accepts a credential, UID, path, or
  interface in the global request.
- Admit at most one global WLAN connection. Automatic selection iterates
  profiles in file order and then interfaces in stable discovery order; manual
  selection uses its exact saved SSID and the same interface rule.
- An empty successful radio enumeration enables `auto-searching` as a valid
  hotplug-wait state. With multiple radios, one preparation failure does not
  fail fast: selection uses the successfully prepared radios in their original
  stable order; an error is terminal only when no discovered radio is usable.
- Keep only operation-local secret copies around one fd-4 child. The persistent
  public states are `disabled`, `auto-searching`, `connected`, and
  `manual-disconnected`; connecting/reconnecting are internal transients only.
- On RF loss, reread the active owner's fixed store and run exactly one normal
  30-second `/sbin/wifi connect` child for the same selected SSID. Success
  returns to `connected`; failure cleans stale L2/L3 state and returns to
  `auto-searching` without a nested retry.
- Explicit disconnect enters `manual-disconnected`, keeps every managed radio
  up and scanning, and suppresses auto-selection. Disable alone lowers radios
  and clears the policy owner.
- Before fd-3 `READY`, and again on normal exit, networkd must enumerate every
  detected WLAN and complete `disconnect`, `search stop`, and `down`, leaving
  policy `disabled`. A normalization failure suppresses `READY` or forces a
  nonzero exit; it is never reported as a successful lifecycle transition.
- Do not request multiple physical boots during implementation.  The separate
  `ws005-p008` final acceptance Phase retains the later consolidated five-run
  campaign.

## Completion boundary

Q071 is finished when each selected Phase is completed or has a precise
`uncleared` result and no selected dependency-ready work remains.  It does not
claim final five-run physical acceptance, AX211 completion, DFS support, or a
general routing socket.

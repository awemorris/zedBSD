# WS005 Phase 002: WLAN v1 control contract

Last updated: 2026-09-05

WSID: `ws005`

Phase ID: `p002`

Combined ID: `ws005-p002`

Status: Complete (`q053`) as historical design closure; 2026-09-05 global
policy amendment frozen for p006/p007/p011 implementation

Parent: [WS005 networking and WLAN](../ws.md)

Tests: [WS005 test index](../tests/README.md)

## Post-completion amendments

Q053 closed the original per-interface command design on 2026-09-01. The user
superseded two parts of that design on 2026-09-04 and 2026-09-05; q053 remains
valid historical design evidence but is not implementation evidence for these
amendments.

The 2026-09-04 amendment moved high-level retry out of the kernel. The kernel
admits asynchronous scan and one-attempt connect generations and emits link
events. One `/sbin/wifi connect` invocation owns one 30-second scan/connect
deadline. The replacement is split across
[`ws004-p044`](../../ws004-hardware/phase044-wlan-async-operation-boundary/phase.md),
[`ws005-p010`](../phase010-wifi-primitive-hardening/phase.md), and
[`ws005-p011`](../phase011-networkd-managed-wlan-reconnect/phase.md).

The 2026-09-05 amendment replaces the public per-interface `net wifi` grammar
with one global WLAN policy. `/sbin/net` is a stateless frontend. `networkd`
holds the active policy effective UID, discovers every present `wlanN`, reads
that UID's p005 credential store when a connection operation needs it, and
keeps no passphrase after that operation. There is at most one managed WLAN
connection system-wide. Public commands never name an interface; direct root
`/sbin/wifi` remains the interface-specific recovery/debug primitive.
The optional `auto` literal is the only public policy spelling: omitting it
creates a manual profile, while an explicit `manual` operand is rejected.

## Objective

Freeze the first WLAN control architecture before any driver, ioctl, command,
credential, or `networkd` implementation begins. The first target is the exact
Japan-market TP-Link Archer T3U Nano retained by WS004 p026. Public V1.0
evidence identifies the RTL8822BU family and software USB `2357:012e` mapping,
but the purchased unit has no printed revision. Its complete retained
`2357:012e`, `bcdDevice=2.10`, `ff/ff/ff`, five-endpoint descriptor is the
binding authority before driver work.

This is a design Phase.  It records the user-selected control topology,
supersedes the earlier pluggable `/sbin/wpa` proposal, assigns ownership to the
following implementation Phases, and freezes the initial safety and resource
bounds which those Phases must preserve.

## Baseline and supersession record

The completed `ws002-p020` baseline provides `/sbin/net`, a root-only
`networkd`, direct `/sbin/ifconfig`, and one-shot `/sbin/dhcpc`.  The historical
WS005 design added a resident, pluggable `/sbin/wpa` child, a versioned
`networkd`--`wpa` conversation, and a root-only `/etc/wpa/` database for the
built-in RTL8822CE.

The user replaced that topology on 2026-08-30.  The following earlier planned
properties are superseded and must not be implemented as compatibility paths:

- a resident or pluggable `/sbin/wpa` backend;
- `networkd`--`wpa` startup negotiation, event protocol, or fake-wpa fixture;
- authentication or association state owned by a userspace WPA daemon;
- `/etc/wpa/` as the initial credential database; and
- RTL8822CE as the first WLAN implementation target.

The built-in RTL8822CE identity remains valid inventory and may become a later
driver target.  Supersession does not renumber or erase the historical
`NET-20`--`NET-24` records.

## Frozen control topology

```text
user or desktop
       |
       v
    /sbin/net -----------------------> networkd
                                         +-- /sbin/ifconfig
                                         +-- /sbin/wifi -> WLAN ioctls
                                         +-- /sbin/dhcpc

root recovery: /sbin/ifconfig ----------> generic network ioctls
root recovery: /sbin/wifi --------------> WLAN ioctls
```

The ownership boundary is fixed as follows:

- the kernel WLAN core and selected driver own scanning, authentication,
  association, transient key installation, carrier truth, disconnect, and the
  Ethernet L2 data path;
- `/sbin/wifi` is a finite, one-shot ioctl wrapper; it does not persist
  credentials, run DHCP, manage routes or DNS, or remain resident;
- `networkd` authenticates the requesting `net` client, serializes mutation,
  owns the active policy UID and global WLAN state, discovers `wlanN` devices,
  reads the policy owner's fixed credential path, invokes fixed child argument
  vectors without a shell, and composes WLAN L2 success with `ifconfig` and
  `dhcpc`;
- `/sbin/net` is the public orchestration client and owns the caller-facing
  `net wifi` grammar, but retains no WLAN policy state; and
- `/sbin/net` writes its own effective-UID profile only for `set-key` and then
  sends a nonsecret `profiles-changed` notification. `networkd` derives the
  active policy UID only from the authenticated peer of `enable`, never from a
  request field, path, or environment variable.

## Frozen public command surface

The direct L2 command family is:

```text
wifi INTERFACE search start
wifi INTERFACE search stop
wifi INTERFACE list
wifi INTERFACE status
wifi INTERFACE up
wifi INTERFACE down
wifi INTERFACE connect SSID PASSPHRASE
wifi INTERFACE disconnect
```

The public, global orchestration family is:

```text
net wifi set-key SSID PASSPHRASE [auto]
net wifi enable
net wifi disable
net wifi list
net wifi connect SSID
net wifi disconnect
```

`wifi` stops at L2.  It must never call `dhcpc`, configure an IPv4 address,
replace a route, or write resolver state. `net wifi enable` establishes the
active policy owner and starts global automatic selection; `networkd` invokes
`dhcpc` only after association.
Ordinary wired `net up`, `net down`, and `net dhcp` retain their established
meanings.

The explicit passphrase argv forms are retained because the user selected
them.  They necessarily expose a secret to shell history and, while the
process exists, potentially to process-argument inspection.  No Phase may
claim otherwise.  The internal `networkd` child path uses a dedicated secret
descriptor defined by `ws005-p006` so the daemon does not add another argv
exposure.

## Frozen state and ordering semantics

- The persistent public managed states are exactly `disabled`,
  `auto-searching`, `connected`, and `manual-disconnected`. `networkd`, not a
  `net` process, owns that state and the active policy UID. `connecting` and
  `reconnecting` may exist only as bounded internal transient states; they do
  not add public persistent policy states.
- `enable` authenticates the peer and atomically makes that peer's effective
  UID the policy owner. A later authorized `enable` replaces the prior owner;
  UID 0 is always authorized to override. It brings up and scans every present
  `wlanN`, reads the owner's store, and considers only `auto` profiles in file
  order.
- There is at most one managed connection. Both automatic and explicit
  selection choose the first WLAN interface in stable discovery order which
  reports a visible supported candidate for the selected profile. The caller
  cannot force an interface through the public grammar.
- `list` returns bounded snapshots for all discovered WLAN interfaces in that
  stable order. It contains no key, credential-presence flag, or policy UID.
- `connect SSID` requires an enabled policy, reads the active owner's store,
  selects exactly that saved profile, and applies the same global interface
  selection rule. A manual profile is eligible even when it lacks `auto`.
- `disconnect` retires the one managed L2/L3 connection and enters
  `manual-disconnected`; it keeps every managed WLAN interface
  administratively up and scanning, but suppresses automatic selection until
  another `enable` or explicit `connect`.
- `disable` cancels scans/connect/DHCP, retires owned L2/L3 state across all
  managed WLAN interfaces, clears the policy owner, and enters `disabled`.
- After RF link loss from `connected`, the kernel reports carrier down and
  `networkd` enters internal `reconnecting`. It reopens the active owner's
  current store and retries the same selected SSID exactly once by running one
  ordinary 30-second `/sbin/wifi connect` child. Success returns to
  `connected` without needlessly replacing coherent L3 ownership. Failure
  retires stale managed L2/L3 state, wipes the operation-local secret, and
  enters persistent `auto-searching`; there is no nested daemon retry.
- `set-key` updates the caller's euid-selected file locally. Omission of
  `auto` means `manual`, and the literal `manual` is not accepted as an
  additional CLI argument. After a successful atomic write, `net` sends a
  nonsecret best-effort `profiles-changed` notification; notification failure
  neither rolls back nor invalidates the completed store update.
- Direct `wifi` search/status/connect/disconnect remain interface-specific
  root recovery operations. They do not establish or mutate networkd's global
  policy state.
- every child stage has an individual diagnostic and all compound operations
  have one total monotonic deadline.  A generic `error` does not replace the
  failing scan, credential, association, carrier, DHCP, route, or resolver
  stage.

## Frozen v1 limits and failure state

The initial bounds are part of the contract, not values to choose while
implementing it:

| Item | V1 limit |
| --- | --- |
| BSS entries in one scan snapshot | `64` |
| Scan admission and completion | `15 s` monotonic maximum |
| One direct L2 `wifi ... connect` | `30 s` monotonic maximum |
| One `dhcpc` stage | `10 s` monotonic maximum |
| One compound `net wifi enable` or `net wifi connect` | `90 s` total monotonic maximum |
| Auto profiles attempted by one selection generation | `4`, in file order |

Every stage is limited by the smaller of its own cap and the remaining
compound deadline.  A retry consumes the existing 90-second budget and never
restarts it.  More than 64 visible BSSes must be represented by an explicit
truncated/overflow indication, never an unbounded allocation or a falsely
complete result.

Validation failures before the first mutation preserve the prior state. Once
an `enable` or `connect` transaction mutates L2 or managed L3, v1 fails clean
rather than attempting to reconnect a prior SSID: it cancels and joins work it
started, disconnects, clears transient keys, removes new or stale DHCP-derived
state owned by that transaction, and enters the state prescribed by p007/p011.
If any retirement step
cannot be proven, the result is `degraded`/rollback-failed and never success.
An `enable` result with no matching auto profile is not this failure path: it
deliberately leaves global state `auto-searching`. Unrelated-interface
routes and resolver contributions are never removed.

## Security boundary

- Non-root desktop clients do not receive direct mutating ioctl authority.
  They use `net` and the authorized `networkd` socket path defined by
  `ws005-p003`.
- Direct mutating `ifconfig` and `wifi` ioctls remain root recovery tools.
  Read-only status and scan-result queries may be available to ordinary users
  only after the p003 privilege audit proves that they do not mutate radio,
  key, or interface state.
- Secrets are never logged, returned by status, included in crash evidence,
  or embedded in `net.conf`.
- The initial plaintext database is an honest plaintext credential store.
  Encryption at rest and an external secret service are later work.
- `networkd` derives caller identity from connection-time kernel credentials,
  never from a UID, home path, or group asserted in a request payload. An
  accepted `enable` stores only that scalar policy UID. UID 0 selects
  `/etc/wifi.conf`; another UID selects its passwd-record home `.wifi.conf`.
  Networkd opens the fixed path when needed and wipes every passphrase copy
  after the one child operation; it never retains a long-lived passphrase.

## Planning inputs and downstream assignments

- `ws002-p020`: synchronous `net`/`networkd`/`dhcpc` baseline and fd 3
  readiness.
- `ws005-p001`: completed physical wired-network milestone which satisfied the
  former `MB-006` resume condition.
- `ws005-p003` is the assigned downstream owner of AF_UNIX peer credentials,
  `root:network` socket ownership, and the direct mutating-ioctl privilege
  boundary; it depends on this frozen design, not the reverse.
- `ws004-p026` is the assigned downstream identity/firmware checkpoint for the
  provisional `2357:012e`/RTL8822BU target.
- `ws004-p027` is the assigned downstream owner of the versioned WLAN UAPI and
  device-independent fixture required before the selected USB driver.

## Ordered work packages

1. Record the supersession decision in M/W planning and remove `MB-006` from
   the active blocking register while retaining a dated history entry.
2. Synchronize the frozen limits, global state, and userspace recovery policy
   into the WS004 UAPI and later WS005 fixtures.
3. Complete `ws005-p003` before allowing an ordinary user or desktop client to
   reach network mutation through `networkd`.
4. Complete the generic WS004 WLAN ioctl and fake-device contract before
   implementing `/sbin/wifi` in `ws005-p004`.
5. Complete the bounded credential model in `ws005-p005` before adding
   automatic selection or `net wifi enable` orchestration.
6. Complete the later `networkd` bridge, compound orchestration,
   selected-driver, and physical-acceptance Phases.  Keep modeled and radio
   evidence separate.

## Design acceptance

The design review must demonstrate all of the following without claiming code
or hardware evidence:

- every public command maps to exactly one owner and a finite L2 or L3 scope;
- no planned path invokes a `/sbin/wpa` daemon or revives its protocol;
- a non-root client reaches privileged composition only through authenticated
  `networkd`, while direct mutating ioctls remain privileged;
- credentials have one root/user storage selection rule and do not enter
  `net.conf`, logs, status, or the internal child argv;
- search generations, association, down, auto-selection, and DHCP ordering
  have deterministic success, timeout, cancellation, and failure states,
  including the exact limits and fail-clean boundary above; and
- exact hardware identity and physical acceptance remain separate from the
  device-independent command and protocol fixtures.

## Completion conditions

- The frozen topology, ownership, command grammar, state ordering, and
  security boundary above are synchronized into WS005, WS004, WS011, and the
  master plan.
- The amended limits and global failure/recovery policy are reflected exactly
  in dependent P-books; implementation does not retain a passphrase between
  child operations or reintroduce interface operands to public `net wifi`.
- The old `wpa` work items and tests are marked superseded rather than deleted
  or reused with different meanings.
- No source change, Queue selection, build result, QEMU result, or physical
  WLAN claim is attributed to this design Phase.

## Reconsideration boundary

Return to design before implementation if the selected adapter requires a
userspace authentication state machine, if secure association cannot be
represented by a bounded driver ioctl contract, or if desktop authorization
would require making the control socket world-writable or installing a setuid
`net`/`wifi` binary.  Do not silently restore `/sbin/wpa`, broaden ioctl
privilege, or parse secrets from human-readable child output.

## Queue boundary and handoff

Q053 selected only this design-closure boundary and did not authorize code.
Future implementation still requires a finite dependency-ready Queue with its
own P-book, timebox, and automatic gates.

## q053 design-closure result (2026-09-01)

At q053 time, the then-current topology, public/direct command grammar,
kernel/driver/command/daemon/client ownership, authenticated privilege
boundary, credential separation, finite failure state, and supersession record
agreed across the master, WS005, WS011, and dependent P-books. The 2026-09-05
global-policy amendment above supersedes the q053 public grammar and profile
reader without rewriting that historical execution result.

Three downstream numeric omissions were corrected: `ws004-p027` now freezes
the 15-second scan and 30-second direct-connect generation budgets, p028 binds
all driver scan work to the same 15-second total, and p029 binds the complete
WPA2 L2 connection to the same 30-second total without retry reset. The
10-second DHCP stage, 90-second compound limit, four auto attempts, 64-entry
snapshot, and then-current 0/1/2/4/8-second kernel same-BSS recovery were
consistent in the q053 downstream P-books. The post-completion amendment above
supersedes that kernel recovery with one ordinary userspace child and does not
rewrite the historical q053 result.

The historical `/sbin/wpa`, `/etc/wpa/`, resident profile loop, kernel
same-BSS recovery, and RTL8822CE-first proposal remain explicitly superseded;
MB-006 remains released.
No source, build, QEMU, radio, or hardware result is attributed to p002.

## q055 identity-policy synchronization (2026-09-01)

After q053 closed this control design, the user supplied the remaining p026
physical fact and decision: the product label is `Archer T3U Nano`, the region
is Japan, and no separate hardware revision is printed. Absence is recorded
rather than inferred from `bcdDevice`; the exact q040 descriptor is
authoritative for this unit. The optional firmware will be separately acquired
and installed through the p036 `userland/firmware/rtl8822b/` recipe;
neither that package decision nor the exact-unit identity changes the frozen
WLAN control topology, limits, or ownership recorded by q053.

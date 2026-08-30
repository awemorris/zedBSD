# WS005 Phase 002: WLAN v1 control contract

Last updated: 2026-08-30

WSID: `ws005`

Phase ID: `p002`

Combined ID: `ws005-p002`

Status: planned; not queued; v1 contract frozen

Parent: [WS005 networking and WLAN](../ws.md)

Tests: [WS005 test index](../tests/README.md)

## Objective

Freeze the first WLAN control architecture before any driver, ioctl, command,
credential, or `networkd` implementation begins.  The first target is a
TP-Link Archer T3U Nano V1.0.  Public primary evidence identifies that product
as RTL8822BU, USB `2357:012e`; the exact physical unit, revision, descriptors,
and firmware remain authoritative and are captured before driver work.

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
  invokes fixed child argument vectors without a shell, and composes WLAN L2
  success with `ifconfig` and `dhcpc`;
- `/sbin/net` is the public orchestration client and owns the caller-facing
  `net wifi` grammar; and
- `/sbin/net` resolves its own effective-UID profile and performs credential
  parsing through shared bounded code; `networkd` never resolves a client home
  directory or chooses a profile on the client's behalf.

## Frozen public command surface

The direct L2 command family is:

```text
wifi INTERFACE search start
wifi INTERFACE search stop
wifi INTERFACE list
wifi INTERFACE status
wifi INTERFACE connect SSID PASSPHRASE
wifi INTERFACE disconnect
```

The public orchestration family is:

```text
net wifi search start INTERFACE
net wifi search stop INTERFACE
net wifi list INTERFACE
net wifi set-key SSID PASSPHRASE [auto]
net wifi up INTERFACE
net wifi down INTERFACE
net wifi connect INTERFACE SSID
```

`wifi` stops at L2.  It must never call `dhcpc`, configure an IPv4 address,
replace a route, or write resolver state.  `net wifi up` is the operation which
eventually performs auto-selection and invokes `dhcpc` after association.
Ordinary wired `net up`, `net down`, and `net dhcp` retain their established
meanings.

The explicit passphrase argv forms are retained because the user selected
them.  They necessarily expose a secret to shell history and, while the
process exists, potentially to process-argument inspection.  No Phase may
claim otherwise.  The internal `networkd` child path uses a dedicated secret
descriptor defined by `ws005-p004` so the daemon does not add another argv
exposure.

## Frozen state and ordering semantics

- `search start` begins or joins an asynchronous scan and is idempotent when
  the same interface is already scanning.
- `search stop` is idempotent.  It stops scan production but does not by itself
  tear down an established association.
- `list` returns one bounded, internally consistent scan snapshot.  It does
  not silently start a scan and does not combine entries from different scan
  generations.
- `status` reports a finite L2 state such as down, idle, searching,
  associating, associated, recovering, or failed.  It never reports a
  passphrase, PSK, PMK, or installed key.
- `connect` selects the named SSID, performs WPA2-Personal authentication with
  CCMP, and returns only after association succeeds or a bounded terminal error
  occurs.  WPA3-SAE, WPA1/TKIP, 802.1X/EAP, WPS, open networks, and downgrade
  to a weaker suite are outside v1.  The primitive does not run DHCP.
- direct `wifi disconnect` cancels pending scan/connect work, disassociates,
  clears transient keys, and publishes carrier down.  It does not mutate IPv4,
  routes, DNS, or the generic `IFF_UP` flag.
- `net wifi list` obtains the bounded snapshot through `networkd` and the
  primitive `wifi list`; a desktop does not bypass `networkd` for this path.
- `net wifi down` later composes `wifi disconnect` with generic interface
  shutdown and managed L3 cleanup.  Its complete route/DNS cleanup transaction
  belongs to the later orchestration Phase.
- `net wifi up` starts scanning if necessary, considers only entries marked
  `auto`, attempts them deterministically, stops scanning after L2 success,
  and only then runs `dhcpc`; command success still requires complete L2+DHCP.
- `net wifi connect` on an already-up interface transactionally retires stale
  managed L3 state, associates with the requested stored profile, and
  reacquires DHCP before reporting success.  It never retains the previous
  SSID's lease after switching networks.
- every child stage has an individual diagnostic and all compound operations
  have one total monotonic deadline.  A generic `error` does not replace the
  failing scan, credential, association, carrier, DHCP, route, or resolver
  stage.
- after link loss from one accepted WPA2 association, the common WLAN core may
  recover only that same BSS using its retained PMK.  It retains no plaintext
  passphrase for recovery, uses retry delays of 0/1/2/4/8 seconds, attempts at
  most five times within one 30-second monotonic window, and keeps secure
  carrier down until reauthentication is complete.  Exhaustion publishes a
  terminal failure; a user or desktop then starts a new policy attempt with
  `net wifi up`.
- v1 has no resident `networkd`/`wifi` loop which searches indefinitely or
  switches among `auto` profiles after link loss.  Explicit disconnect/down
  cancels same-BSS recovery and clears the retained PMK and transient keys.

## Frozen v1 limits and failure state

The initial bounds are part of the contract, not values to choose while
implementing it:

| Item | V1 limit |
| --- | --- |
| BSS entries in one scan snapshot | `64` |
| Scan admission and completion | `15 s` monotonic maximum |
| One direct L2 `wifi ... connect` | `30 s` monotonic maximum |
| One same-BSS kernel recovery window | delays `0/1/2/4/8 s`, at most `5` failed attempts and `30 s` total |
| One `dhcpc` stage | `10 s` monotonic maximum |
| One compound `net wifi up` or `net wifi connect` | `90 s` total monotonic maximum |
| Auto profiles attempted by one `net wifi up` | `4`, in file order |

Every stage is limited by the smaller of its own cap and the remaining
compound deadline.  A retry consumes the existing 90-second budget and never
restarts it.  More than 64 visible BSSes must be represented by an explicit
truncated/overflow indication, never an unbounded allocation or a falsely
complete result.

Validation failures before the first mutation preserve the prior state.  Once
an `up` or `connect` transaction mutates L2 or managed L3, v1 fails clean
rather than attempting to reconnect a prior SSID: it cancels and joins work it
started, disconnects, clears transient keys, removes new or stale DHCP-derived
state owned by that interface transaction, stops search, and leaves the
interface administratively up for an explicit retry.  If any retirement step
cannot be proven, the result is `degraded`/rollback-failed and never success.
An `up` result of "no matching auto profile" is not this failure path: it
deliberately leaves the interface up and search active.  Unrelated-interface
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
  never from a UID, home path, or group asserted in a request payload.  The
  `net` client selects `/etc/wifi.conf` when its effective UID is zero and its
  passwd-record home `~/.wifi.conf` otherwise, then sends only the selected,
  bounded values needed by the requested operation.

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
2. Synchronize the frozen limits, fail-clean state, and bounded same-BSS
   recovery policy into the WS004 UAPI and later WS005 fixtures.
3. Complete `ws005-p003` before allowing an ordinary user or desktop client to
   reach network mutation through `networkd`.
4. Complete the generic WS004 WLAN ioctl and fake-device contract before
   implementing `/sbin/wifi` in `ws005-p004`.
5. Complete the bounded credential model in `ws005-p005` before adding
   auto-selection or `net wifi up` orchestration.
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
- The limits and failure/recovery policy are reflected exactly in dependent
  P-books; implementation does not silently widen a bound or add a resident
  profile-selection loop.
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

This Phase is not in a Queue and does not authorize code.  It has no remaining
human design gate; propose only a finite dependency-ready implementation slice
with its own P-book, timebox, automatic gates, and explicit Queue approval.

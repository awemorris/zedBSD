# WS005 Phase 011: networkd global WLAN policy and recovery

Last updated: 2026-09-05

WSID: `ws005`

Phase ID: `p011`

Combined ID: `ws005-p011`

Status: complete (`q071`); the global policy, no-retained-passphrase contract,
focused recovery gates, and combined user-accepted physical result pass

Parent: [WS005 networking and WLAN](../ws.md)

Protocol dependency: [networkd Wi-Fi control protocol](../phase006-networkd-wifi-protocol/phase.md)

Orchestration dependency: [global `net wifi` orchestration](../phase007-net-wifi-orchestration/phase.md)

Primitive retry dependency: [wifi hardening](../phase010-wifi-primitive-hardening/phase.md)

Kernel event dependency: [WS004 p044](../../ws004-hardware/phase044-wlan-async-operation-boundary/phase.md)

Tests: [WS005 test index](../tests/README.md)

## Objective

Make root `networkd` the sole resident owner of one global WLAN policy:

```text
/sbin/net -> /run/networkd.sock -> networkd -> interface-specific /sbin/wifi
                                      `-----> /sbin/ifconfig and /sbin/dhcpc
```

Networkd holds the authenticated active policy UID, discovers all `wlanN`,
permits at most one managed WLAN connection, consumes p044 link events, and
owns the four persistent public states `disabled`, `auto-searching`,
`connected`, and `manual-disconnected`. Bounded `connecting` and
`reconnecting` states may exist internally only while a transaction is
active. `net` is stateless. The p005 file is the only long-lived passphrase
owner; networkd reopens it when needed and wipes the selected key after each
child.

## Supersession record

The first unimplemented p011 draft retained one selected passphrase in
networkd memory and owned a daemon retry loop. Both properties are
superseded. The final 2026-09-05 contract retains only a policy UID and
nonsecret connection facts. On RF loss it runs exactly one ordinary 30-second
`/sbin/wifi connect` child for the same selected SSID after reopening the
store; child failure cleans the stale connection and settles in
`auto-searching`. No compatibility state or migration is required.

## Ownership and secret lifetime

The resident global record may contain only:

- state and authenticated active policy UID;
- policy generation and interface-discovery generation;
- automatic/manual connection origin and nonsecret desired/selected SSID;
- selected interface name, ifindex, device-generation identity, and event
  sequence; and
- exact managed DHCP address/route/resolver ownership needed for cleanup.

It contains no passphrase, PMK, PTK, GTK, nonce, complete profile model, client
home, or caller-supplied path. Before each connection child, networkd derives
the fixed p005 path from the active UID, takes the shared lock, validates and
parses one complete generation, copies only the selected passphrase into a
bounded operation buffer, closes/clears the model, passes the key on fd 4, and
wipes the operation buffer after child termination. Before fd-3 `READY` on
service start, networkd enumerates every detected WLAN and completes
`disconnect`, `search stop`, and `down`; only then does it publish the
credential-free `disabled` state. Normal exit repeats that normalization and
clears the owner. A normalization failure suppresses readiness or forces a
nonzero exit rather than claiming lifecycle success.

## Persistent and transient state model

Only the four uppercase states below are persistent public policy states.
`CONNECTING` and `RECONNECTING` are internal transaction markers and must
settle within the owning operation deadline.

```text
DISABLED
  -> AUTO_SEARCHING          authorized enable installs policy UID

AUTO_SEARCHING
  -> CONNECTING              automatic or explicit selection starts
  -> MANUAL_DISCONNECTED     explicit disconnect
  -> DISABLED                disable

CONNECTING                   internal transient
  -> CONNECTED               selected interface completes L2 + DHCP
  -> AUTO_SEARCHING          automatic attempt fails cleanly
  -> MANUAL_DISCONNECTED     explicit connect fails cleanly

CONNECTED
  -> RECONNECTING            RF/carrier loss
  -> AUTO_SEARCHING          selected-device removal after cleanup
  -> MANUAL_DISCONNECTED     explicit disconnect
  -> AUTO_SEARCHING          authorized enable switches policy owner
  -> DISABLED                disable

RECONNECTING                 internal transient
  -> CONNECTED               one same-SSID wifi child succeeds
  -> AUTO_SEARCHING          that child fails and cleanup completes

MANUAL_DISCONNECTED
  -> AUTO_SEARCHING          enable
  -> CONNECTING              explicit connect starts
  -> DISABLED                disable
```

An authorized `enable` first validates its authenticated peer euid's fixed
store and completes a stable radio enumeration without mutating the current
policy. After that preflight succeeds, owner replacement cancels and joins old
work, retires the old managed connection, and prepares the enumerated radios.
Only successful policy preparation atomically publishes the new owner. A later
authorized enable follows the same order, and UID 0 can always override.

`manual-disconnected` deliberately suppresses automatic selection while
retaining the policy UID. Every managed WLAN interface remains up and scanning
so `list` and a later manual connection have current snapshots, but scan
completion cannot initiate automatic association in this state. Only `enable`
or explicit `connect SSID` resumes connection work. `disable` clears the owner
and all WLAN policy state.

## Discovery and one-connection arbitration

For each selection generation, enumerate present WLAN devices in stable kernel
discovery order. Attempt to bring up and scan every device. Failure to prepare
one radio does not fail fast or reorder the generation: retain each successful
radio in its original stable position and select only among those radios. A
nonempty generation fails preparation only if no radio is usable, but an empty
successful enumeration is a valid enabled `auto-searching` state waiting for
hotplug. Admit at most one association:

- automatic mode considers `auto` profiles in p005 file order, then selects
  the first stable-order WLAN whose snapshot reports a visible supported
  candidate for that profile;
- manual `connect SSID` selects that exact saved profile regardless of its
  `auto` flag, then the first stable-order WLAN which reports it; and
- once a winner begins association, later interfaces cannot preempt it because
  of completion time, RSSI, or lexical name. Cancel losing scans after commit.

Device removal or identity mismatch invalidates the selection. A later device
or reused `wlanN` is a fresh discovery generation and cannot inherit link, L3,
child, or secret ownership.

## Event-driven recovery

Networkd opens the read-only p044 route socket before its initial interface
snapshot and polls it with `/run/networkd.sock` and active child pipes. Events
are wakeups plus stable interface identity; canonical flags/WLAN state are
reread before a transition. Overflow triggers a full resnapshot, never a blind
connect.

A genuine carrier transition from `connected` to down closes the controlled
port in the kernel and moves policy to internal `reconnecting`. Networkd
retains the policy UID, same selected SSID, selected interface/device identity,
connection origin, and exact L3 ownership, then reopens the current credential
store. If the exact profile still exists and is valid, it copies only that
passphrase and starts exactly one ordinary p010 `/sbin/wifi connect` child for
the still-valid selected interface generation. That child alone owns its
30-second scan/select/connect retry; networkd and the kernel do not wrap it in
another retry or switch to another profile during this attempt.

If the profile vanished, the device identity changed, or the one child fails,
networkd disconnects any partial L2 state, removes stale owned L3 state, wipes
the operation-local secret, and enters persistent `auto-searching`. There is
no immediate second recovery child. Later automatic selection uses ordinary
profile-file order and then stable radio discovery order. If the one child
succeeds on the same network, coherent L3 ownership may remain and state
returns to `connected`; a changed SSID/interface in a later selection requires
fresh DHCP before entering `connected`.

## Public-operation integration

- `enable` installs/switches policy UID and starts global automatic search.
- `disable` cancels all work, retires the managed connection/L3, brings managed
  WLAN interfaces down, clears the owner, and enters `disabled`.
- `list` aggregates per-interface machine snapshots without changing policy.
- `connect SSID` uses the active owner's exact saved profile and the global
  interface rule. It never accepts a passphrase or interface from `net`.
- `disconnect` stops connection/recovery work, retires managed L2/L3, keeps
  every managed WLAN interface up and scanning, and enters
  `manual-disconnected` without clearing the active UID. Updated scan results
  cannot trigger auto association until `enable`.
- a successful local `set-key` sends empty `profiles-changed`. Only a notice
  from the active policy UID invalidates current parsed metadata. It does not
  disrupt `connected`; `auto-searching` may begin a new selection generation.

Direct root `/sbin/wifi` commands do not create or mutate networkd's policy.

## Child and event-loop integration

Use one bounded poll-driven loop. Each interface-specific wifi child inherits
only its machine stdout, bounded diagnostic stderr, and fd-4 secret. It must
not inherit the listener, accepted client, readiness fd 3, route socket, store
descriptor, or another child's pipes. P006's output ceilings, one-second
termination grace, reaping, machine-record validation, and redaction apply.

Global mutation remains single-flight. `disable` and `disconnect` are admitted
as cancellation. Owner-switching `enable` validates the new store and stable
radio snapshot before cancelling old work or publishing the new owner.
Repeated link-down events for one device generation are coalesced; link-up
without an owned successful child cannot fabricate `connected`.

## Implementation sequence

1. Replace the old per-interface managed table and passphrase member with one
   global state record containing only the fields allowed above.
2. Add authenticated pre-mutation policy preparation, owner
   install/switch/clear, and start/normal-exit radio normalization whose failure
   cannot publish readiness or successful termination.
3. Add stable all-WLAN discovery generations, valid zero-radio hotplug wait,
   per-radio preparation isolation, and exactly-one-connection arbitration
   shared by automatic and manual selection.
4. Move p005 store reads into networkd, add operation-local secret buffers and
   fd-4 transfer, and prove no ZNV2 or resident secret remains.
5. Integrate p044 route events, overflow resnapshot, profiles-changed, hotplug,
   the four persistent states, connecting/reconnecting transients, the single
   same-SSID recovery child, and fresh-DHCP rules.
6. Add deterministic policy, discovery, event, store, clock, child, L3,
   cleanup, restart, and secret-lifetime fixtures.
7. Run focused ordinary/sanitizer/analyzer tests, supported builds/QEMU, wired
   ZNV2/fd-3 regressions, and p010 direct-wifi regressions. Do not run aggregate
   `make check`.

## Verification

- Zero/one/multiple WLAN interfaces preserve stable discovery order and never
  yield more than one managed association. Zero radios enables a valid
  auto-searching hotplug wait; one failed radio does not prevent selection from
  successfully prepared radios later in the same stable generation.
- Automatic profiles follow file order; manual selection uses its exact saved
  SSID; both pick the first eligible stable-order interface.
- Root/nonroot enable, pre-mutation store/enumeration failure, owner switch,
  root override, wrong payload UID/path, service restart, disable, and
  manual-disconnected transitions are exact; a failed prospective policy does
  not replace the current owner, and explicit disconnect leaves radios
  up/scanning while suppressing auto.
- Service start publishes fd-3 readiness only after all detected WLAN radios
  are disconnected, scan-stopped, and down. Normal exit repeats the operation;
  either lifecycle boundary reports failure instead of a false success if any
  radio cannot be normalized.
- Store missing/invalid/replaced/unsafe/locked and profile removed/changed
  cases fail without using stale credential material.
- Every selected passphrase exists only in bounded reader/operation/fd-4
  buffers and is absent from resident state, ZNV2, argv, environment, logs,
  machine output, diagnostics, core evidence, and post-operation scans.
- RF loss, the one same-selected-SSID 30-second child, success/failure
  settlement, removal, duplicate/stale/out-of-order events, overflow,
  resnapshot, profiles-changed, explicit cancellation, scan generation
  replacement, and no-nested-retry behavior are deterministic.
- Same-network recovery success preserves coherent L3; failed recovery removes
  stale L3; later success or changed SSID/interface performs fresh DHCP.
- Every child, scan, callback, route event, L3 object, and secret is retired or
  reported as bounded `degraded`; unrelated wired/interface state survives.

## Completion conditions

- The four persistent global states and active authenticated UID are resident
  only in networkd; connecting/reconnecting are bounded internal transients,
  `net` is stateless, and public commands have no interface operand.
- Exactly one connection is selected by profile file order and stable WLAN
  discovery order.
- Enable installs its new owner only after store and enumeration validation;
  empty enumeration waits successfully for hotplug, while partial preparation
  failure preserves every usable radio's stable selection position.
- Startup and normal exit reach physically normalized `disabled`, or report a
  truthful readiness/exit failure.
- The p005 file is the only long-lived passphrase owner and no networkd policy
  record retains a key.
- RF loss transitions `connected -> reconnecting`, invokes exactly one
  p010-bounded child for the same selected SSID, and settles in `connected` on
  success or cleans and enters `auto-searching` on failure. It cannot spin or
  revive after explicit disconnect/disable.
- Focused automatic and regression gates pass. P008 owns the later combined
  physical observation and final repeatability campaign.

## Interruption and resumption

Return to the owning Phase if stable device discovery, policy-owner path
selection, route-event identity, asynchronous cancellation, child reaping, or
L3 cleanup cannot satisfy this contract. Do not add a public interface/UID/path
operand, cache a passphrase, send a secret through ZNV2, restore kernel
high-level reconnect, or introduce `/sbin/wpa` as a workaround.

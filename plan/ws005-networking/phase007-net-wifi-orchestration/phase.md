# WS005 Phase 007: global `net wifi` orchestration

Last updated: 2026-09-05

Phase ID: `ws005-p007`

Status: complete (`q071`); the 2026-09-05 global command and ownership
amendment, focused gates, and combined user-accepted physical result pass

Parent: [WS005](../ws.md)

Protocol dependency: [networkd Wi-Fi control protocol](../phase006-networkd-wifi-protocol/phase.md)

Tests: [WS005 test index](../tests/README.md)

Architecture: [WLAN v1 control contract](../phase002-wlan-v1-contract/phase.md)

Authentication: [AF_UNIX peer credentials](../phase003-unix-peer-credentials/phase.md)

Primitive: [wifi ioctl command](../phase004-wifi-ioctl-command/phase.md)

Profiles: [Wi-Fi credential store](../phase005-wifi-credential-store/phase.md)

## Objective

Implement one global WLAN policy behind a stateless `/sbin/net` frontend:

```text
user or desktop -> /sbin/net -> /run/networkd.sock -> networkd
                                                    +-- /sbin/ifconfig
                                                    +-- /sbin/wifi
                                                    `-- /sbin/dhcpc
```

Public `net wifi` commands never name `wlanN`. Networkd discovers all WLAN
interfaces, permits at most one managed connection system-wide, stores the
active policy effective UID, and owns the persistent state machine. It reads
the fixed p005 store for that UID only when selection or reconnection requires
it. `/sbin/net` never caches policy, sends a credential, or tells networkd
which file or UID to use.

`/sbin/wifi` remains an interface-specific one-shot L2 primitive. Networkd
passes a selected passphrase to one private child on descriptor 4 and wipes
every in-memory copy at the end of that operation. Policy state may retain a
UID, nonsecret desired SSID, selected interface identity, connection mode, and
owned L3 facts; it must never retain a passphrase or PMK between operations.

## Supersession record

The q053/p007 draft used `search start/stop INTERFACE`, `list INTERFACE`, `up
INTERFACE`, `down INTERFACE`, and `connect INTERFACE SSID`, and made `net` read
and transmit selected profiles. The user superseded that unreleased public
surface on 2026-09-05. There is no compatibility parser or alias for those
forms. The p005 file format, direct `/sbin/wifi` interface grammar, ZNV2
framing, authenticated socket, child fd-4 boundary, and wired commands remain.

## Dependencies

- WS004 p044 provides asynchronous per-interface scan and one-attempt connect
  generations, terminal carrier loss, and stable interface events without a
  hidden kernel high-level reconnect.
- WS005 p005 provides the one root/per-user file grammar, safe euid/UID path
  selection, lock, checked reader, atomic writer, file order, and secret wipe.
- WS005 p006 provides authenticated ZNV2 global WLAN opcodes, daemon-side
  policy-store reads, the private `WIFI1` child protocol, descriptor 4,
  bounded output, timeout, cancellation, and reaping.
- WS005 p010 provides the one-command 30-second `/sbin/wifi connect`
  scan/select/connect sequence consumed unchanged by networkd.
- Existing `/sbin/ifconfig`, `/sbin/dhcpc`, wired `net`/networkd behavior, and
  fd-3 readiness remain regression baselines.
- `IFF_RUNNING` is true only after secure L2 authorization is usable for
  Ethernet-II traffic; finding a BSS or beginning authentication cannot start
  DHCP.

## Public command grammar

The complete WLAN public grammar is:

```text
net wifi set-key <SSID> <passphrase> [auto]
net wifi enable
net wifi disable
net wifi list
net wifi connect <SSID>
net wifi disconnect
```

There are no public interface operands. Every command has exactly the arity
above; the obsolete per-interface spellings fail before a request or mutation.
SSID is one argv operand and ordinary shell quoting preserves whitespace.
`auto` is the only `set-key` option. Omission writes `manual` and clears a
previous `auto` flag. The literal `manual` is not a public option and is
rejected as an extra operand.

The interactive `net` console exposes the same grammar through shared
handlers. It must parse quoted/escaped SSIDs, suppress the secret-bearing
`set-key` line from history, and wipe its mutable passphrase copy. `net` exits
without retaining the active owner, selected SSID, interface, link, or retry
state.

## Policy owner and authorization

Networkd obtains every request's PID/effective UID/effective GID from p003's
connection-time peer snapshot. A request payload cannot contain a UID, user
name, home, profile path, interface, or passphrase.

A prospective `enable` first derives and validates its peer effective UID's
fixed store and completes one stable radio enumeration without changing the
current policy. After that preflight succeeds, networkd cancels and joins old
work, retires old managed L2/L3 state, wipes operation-local secrets, and
prepares the enumerated radios. It atomically publishes the new active policy
UID only after that policy preparation succeeds. A later authorized `enable`
follows the same order; UID 0 is always authorized to override.

The active UID selects exactly one store:

- UID 0: `/etc/wifi.conf`;
- nonzero UID: `.wifi.conf` below the passwd-record home resolved for that UID.

Networkd applies p005's stable-directory, no-follow, owner/mode, shared-lock,
size, grammar, and redaction rules. It never trusts `HOME`, its service
environment, a request path, or a request UID. At service start, networkd
enumerates every detected WLAN and completes primitive `disconnect`, `search
stop`, and `down` before publishing fd-3 `READY` in `disabled`; a failure
publishes no false readiness. Normal service exit repeats that normalization,
clears the active UID, and returns nonzero rather than claiming success if any
radio cannot be normalized. Boot-time policy is outside this Phase.

## Global interface and candidate selection

Networkd snapshots every present `wlanN` in stable kernel discovery order for
one selection generation. It attempts to bring up and scan every enumerated
device through the interface-specific primitives. One radio's preparation
failure does not fail fast or reorder the generation: selection considers only
successfully prepared radios in their original stable order. A nonempty
generation fails preparation only if none of its radios is usable. At most one
device may proceed to a managed association; every other scan remains
non-associated and is cancelled after a winner commits.

A successful empty enumeration is not a device error for `enable`: networkd
publishes the prepared owner in `auto-searching` and waits for hotplug. An
explicit `connect SSID` still requires a currently usable radio.

Automatic selection uses two deterministic levels:

1. consider `auto` profiles in credential-file order, with at most four actual
   connection attempts in one generation; then
2. for that profile, select the first WLAN interface in stable discovery order
   whose snapshot reports a visible supported candidate.

Explicit `connect SSID` uses the exact matching saved profile regardless of
its `auto`/`manual` flag and applies the same stable interface rule. The public
caller cannot select an interface, and later/faster completion, stronger RSSI
on a later interface, or lexical interface spelling cannot preempt the first
eligible interface in the stable order. The primitive remains responsible for
selecting a supported BSS on its chosen interface.

Hotplug creates a new discovery generation. A removed or identity-reused
device cannot inherit selected-interface, child, key, or L3 ownership. A newly
inserted device becomes eligible on the next auto-search generation without
changing the policy UID.

## Persistent and transient state model

The only persistent public policy states are `disabled`, `auto-searching`,
`connected`, and `manual-disconnected`. `connecting` and `reconnecting` are
bounded internal transients used while one child transaction is active; they
must settle to one of the four persistent states and are not new public policy
modes.

```text
DISABLED
  -> AUTO_SEARCHING          authorized enable

AUTO_SEARCHING
  -> CONNECTING              automatic or explicit selection starts
  -> MANUAL_DISCONNECTED     explicit disconnect
  -> DISABLED                disable

CONNECTING                   internal transient
  -> CONNECTED               L2 + DHCP success
  -> AUTO_SEARCHING          automatic attempt fails cleanly
  -> MANUAL_DISCONNECTED     explicit connect fails cleanly

CONNECTED
  -> RECONNECTING            RF/carrier loss
  -> MANUAL_DISCONNECTED     explicit disconnect
  -> DISABLED                disable

RECONNECTING                 internal transient
  -> CONNECTED               one same-SSID wifi child succeeds
  -> AUTO_SEARCHING          that one child fails and cleanup completes

MANUAL_DISCONNECTED
  -> AUTO_SEARCHING          enable
  -> CONNECTING              explicit connect starts
  -> DISABLED                disable
```

- `disabled` has no policy UID, active scans, managed connection, or owned WLAN
  L3 state.
- `auto-searching` has an active policy UID and global scan/selection policy,
  but no usable managed connection. It may retain a nonsecret desired SSID and
  mode for recovery, never a passphrase.
- `connected` identifies exactly one interface generation, selected SSID,
  automatic/manual origin, and owned DHCP/L3 state.
- `manual-disconnected` preserves the policy UID but suppresses automatic
  connection until `enable` or an explicit `connect`; WLAN interfaces remain
  administratively up with scanning active.

One `/sbin/wifi connect` invocation owns its complete 30-second retry; neither
networkd nor the kernel nests another retry around that active child. P011
defines bounded background rescan/recovery scheduling after carrier loss and
must avoid a tight retry loop.

## Operation contracts

### `set-key`

`net` selects its own effective-UID store, validates and atomically replaces or
appends the record, and wipes the argv/model secret. Omission of `auto` writes
`manual`. Only after the file commit succeeds, it sends an empty, nonsecret
`profiles-changed` request. Notification has one five-second total deadline;
failure produces at most a bounded warning, never rolls back the file, and does
not change the successful command result.

Networkd uses the notification peer's authenticated UID. If it is not the
active policy UID, the notification has no policy effect. For the active UID it
invalidates only parsed nonsecret metadata; an ongoing child is not supplied a
new secret and an existing connection is not disrupted. The next selection or
recovery reopens the complete current file.

### `enable`

`enable` validates the prospective owner's current store and stable radio
enumeration before any policy mutation. It then retires the old policy,
brings up and scans every usable WLAN interface, and installs the prepared
owner in `auto-searching` only after radio preparation succeeds. It then
considers only `auto` records in file order, applies the global interface rule,
invokes one private wifi child per permitted attempt, and runs `dhcpc` only
after secure L2 success. Zero radios is a successful enabled hotplug-wait
state; partial preparation continues on usable radios in their original stable
order.

The initiating request has one 90-second compound deadline. Success returns
the selected SSID, chosen interface, and nonsecret L3 result. No configured or
visible automatic candidate is a truthful `auto-searching` result, not a false
connection success and not a reason to discard the active policy.

### `disable`

`disable` cancels and joins every scan/connect/DHCP/recovery operation, invokes
primitive disconnect where required, removes only globally managed WLAN
addresses/routes/resolver contributions, brings managed WLAN interfaces down,
clears the policy UID and nonsecret desired state, and enters `disabled`.
Success requires complete retirement; otherwise return `degraded` with exact
remaining ownership.

### `list`

`list` returns one bounded aggregate view over all discovered WLAN interfaces
in stable discovery order. Every record identifies its interface plus SSID,
BSSID, RSSI, channel/band, supported security, scan state, and generation.
Scanning, complete-with-zero-results, failed, removed, and aggregate truncation
are distinguishable. It does not expose credentials or silently change the
policy owner. If disabled, it reports retained/current snapshots without
starting a global policy.

### `connect SSID`

`connect` requires an active policy UID and an exact saved profile in that
owner's current store. Networkd enters a bounded manual transaction, scans all
eligible WLAN interfaces as needed, selects the first matching interface by
the stable rule, passes the operation-local key on fd 4, and reacquires DHCP.
The complete transaction has one 90-second deadline, including the 30-second
wifi child and 10-second DHCP caps.

Success enters `connected` and retains only nonsecret policy/link/L3 facts.
Failure after cleanup enters `manual-disconnected`; an unknown profile fails
before mutation. It never falls back to another SSID and never sends a
passphrase over ZNV2.

### `disconnect`

`disconnect` cancels and joins active automatic/manual work, disconnects the
one managed link, removes its managed L3 state, keeps every managed WLAN
interface administratively up and scanning, and enters
`manual-disconnected`. Automatic candidate selection remains suppressed in
that state even as scan snapshots update. It preserves the active policy UID
so a later explicit `connect` uses the same owner's store. It is idempotent
only after complete retirement.

### RF link loss

Carrier loss from `connected` immediately closes the controlled port in the
kernel and emits p044's interface event. Networkd enters internal
`reconnecting`, retains the active UID and same selected SSID, reopens the
current store, and starts exactly one ordinary p010 `/sbin/wifi connect` child
with that profile's operation-local fd-4 secret. That child alone owns its
normal 30-second retry. Networkd does not wrap it in another retry or switch
profiles during this recovery attempt.

Success returns to `connected` and preserves coherent same-network L3 state.
Failure retires stale managed L2/L3 ownership, wipes every operation-local
secret, and settles in `auto-searching`; later automatic selection follows the
ordinary profile-file/stable-radio order. Explicit `disconnect` instead
enters `manual-disconnected`, leaves radios up and scanning, and suppresses
that automatic selection; `disable` removes the policy completely.

## Transaction and fail-clean model

Networkd is globally single-flight for mutation in v1. Before mutation it
validates authorization, policy UID/store, interface snapshot, SSID and child
contracts. It records administrative flags, scan generations, selected device
generation, secure carrier, managed addresses/routes/resolver ownership, and
the current global state.

After mutation, timeout, cancellation, client loss, malformed child output,
device removal, authentication failure, or DHCP failure performs one ordered
cleanup:

1. cancel and join DHCP and every asynchronous scan/connect generation owned
   by the transaction;
2. invoke primitive disconnect, close the controlled port, clear transient
   keys, and prove carrier down;
3. remove only transaction-owned addresses, routes, and resolver state while
   preserving other interfaces;
4. wipe store/model/fd-4 secret copies; and
5. publish the prescribed global terminal state or `degraded` with exact
   unretired ownership.

Killing a child or merely running `ifconfig down` is not cleanup proof.

## Planned implementation

1. Replace the unreleased per-interface WLAN opcodes and frontend parser with
   the exact global grammar; add strict old-form rejection fixtures.
2. Make `net` stateless. Retain p005 local `set-key`, then add the empty
   five-second best-effort `profiles-changed` notification.
3. Add networkd's authenticated policy-UID owner and fixed p005 store reader;
   delete ZNV2 profile/passphrase/interface input for global WLAN operations.
4. Add stable all-`wlanN` discovery snapshots, aggregate list records, one-link
   arbitration, profile-file order, and stable interface selection.
5. Implement enable/disable/connect/disconnect transactions, the four
   persistent states, and bounded internal connecting/reconnecting transients
   using only absolute `/sbin/ifconfig`, `/sbin/wifi`, and `/sbin/dhcpc`
   children.
6. Hand p011 only nonsecret active UID/desired SSID/interface/L3 facts and
   route events; p011 must reopen the store for every recovery child.
7. Inject every parser/auth/store/discovery/child/DHCP/cleanup/state-transition
   failure and run focused host/native, sanitizer/analyzer, wired/fd-3, build,
   and bounded QEMU gates. Do not run aggregate `make check`.

## Verification contract

Automatic evidence includes:

- exact global arity, no interface operands, quoted/boundary SSIDs, manual
  default, old per-interface rejection, and matching interactive handlers;
- a stateless net process, no profile/passphrase ZNV2 field, no payload UID or
  path, and no secret in argv except the unavoidable public `set-key` process;
- root/nonroot policy ownership from peer credentials, pre-mutation store and
  enumeration validation, owner replacement only after preparation, root
  override, fixed-path safe daemon reads, and start/normal-exit normalization
  whose failure cannot masquerade as fd-3 readiness or exit success;
- successful store update with notification success, unavailable daemon,
  timeout, rejection, wrong active UID, and proof that notification failure
  neither rolls back the file nor changes its successful result;
- zero/one/multiple WLAN devices, zero-radio enabled hotplug wait, partial
  multi-radio preparation failure without fail-fast or reordering, stable
  discovery order, competing scan completion order, hotplug/removal/name
  reuse, profile-file order, four-attempt cap, and exactly one associated
  interface;
- global list empty/scanning/complete/failed/truncated records with interface
  identity and no credential information;
- every transition among disabled, auto-searching, connected, and
  manual-disconnected plus bounded connecting/reconnecting transients,
  including one same-SSID RF retry, its success/failure settlements, explicit
  suppression with scans retained, owner switch, and service restart;
- exact absolute child argv, fd 4, store/read and secret-wipe lifetime,
  30-second wifi, 10-second DHCP, 90-second compound, cancellation, reaping,
  fail-clean, degraded, and no nested retry;
- no stale DHCP address, route, resolver, device generation, desired SSID, or
  credential survives its ownership boundary; and
- migrated wired ZNV2, direct `ifconfig`/`wifi`, `dhcpc`, fd-3 readiness, build,
  and supported QEMU regressions.

No physical adapter or access point is required by p007.

## Completion conditions

- Only the six global public forms are accepted and `net` retains no policy or
  credentials after each command.
- Networkd alone owns the authenticated active policy UID, four persistent
  global states, bounded internal transients, all-interface
  discovery/selection, one connection, and L3 transaction.
- The p005 store is the sole long-lived passphrase owner. Networkd rereads it
  by fixed UID-derived path, passes one selected secret on fd 4, and wipes it
  after each child; ZNV2 contains no secret.
- Automatic profiles follow file order, explicit connection uses the exact
  saved profile, and both choose the first eligible interface in stable
  discovery order.
- Enable publishes its new owner only after the new store and radio enumeration
  validate; zero radios waits successfully in `auto-searching`, and a failed
  radio cannot suppress usable later radios in the same stable generation.
- Startup and normal exit leave every detected WLAN disconnected, scan-stopped,
  down, and policy-disabled, or report lifecycle failure truthfully.
- Every operation and cleanup is bounded, truthful, cancellation-safe, and
  regression-tested before p008 requests physical work.

## Interruption and resumption

Return a blocker to its owning lower Phase if peer credentials, the safe p005
reader, asynchronous kernel cancellation, route events, child retirement, or
L3 ownership cannot satisfy this contract. Do not restore public interface
arguments, make `net` stateful, send secrets in ZNV2, cache a passphrase in
networkd, or revive `/sbin/wpa` as a workaround.

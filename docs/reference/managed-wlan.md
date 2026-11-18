# Managed WLAN administration

Status: current for the managed WLAN interface of `/sbin/net`

The supported high-level interface is `/sbin/net wifi`. It saves
WPA2-Personal/CCMP networks in per-account credential stores and manages one
global WLAN policy through root `networkd`, which joins a saved network and
acquires IPv4 configuration only after the controlled port is authorized.
No high-level command accepts an interface operand.

The commands and configuration model build for the maintained amd64 and i386
base systems. The radios used with this interface are the RTL8822BU (2.4 GHz
and Japan W52 5 GHz) and the Intel AX211; either may become `wlan0` or
`wlan1`. This does not claim DFS, W53/W56, WPA3, roaming, AP mode, or
universal freedom from firmware initialization failures.

## Command grammar

The complete public grammar is:

```text
net wifi add SSID [--password PASSWORD] [--auto yes|no]
net wifi modify SSID [--password PASSWORD|-] [--auto yes|no]
net wifi delete SSID
net wifi enable
net wifi disable
net wifi list
net wifi connect SSID
net wifi disconnect
net wifi session open|close [account]
```

These forms are available both as arguments to `/sbin/net` and within the
argument-free interactive `net>` console, where the leading `net` is left
out. Supplying `wlan0`, another interface name, a file path, or a credential
to `enable`, `disable`, `list`, `connect` or `disconnect` is a syntax error.
`connect` takes an SSID of 1 to 32 bytes.

## Saving networks

`add`, `modify` and `delete` change the credential store of the account that
runs the command (see [Credential stores and format](#credential-stores-and-format));
an account never changes another account's saved networks.

- `add SSID` saves a network that is not saved yet and appends it to the
  store. `--auto` defaults to `yes`, which makes the network eligible for
  automatic joining; `--auto no` saves it for an explicit `connect` only.
  Adding an SSID that is already saved fails with `Wi-Fi network SSID is
  saved already; use net wifi modify to change it`.
- `modify SSID` changes only what it is given: `--password` replaces the key,
  `--auto` replaces the mode, and at least one of them is required. The
  profile keeps its place in the store. Modifying an SSID that is not saved
  fails with `Wi-Fi network SSID is not saved`.
- `delete SSID` forgets a saved network; the later profiles keep their order.
  It takes no option. If the current managed connection was made with the
  deleted profile's key, `networkd` leaves that network and searches again.
- Each option may be given once. An unknown option, a missing value, or an
  `--auto` value other than `yes` or `no` is a syntax error.

The key need not appear on the command line. `add` without `--password`, and
`modify` with `--password -`, read the key from standard input: on a
terminal after a `Password:` prompt with echo turned off, otherwise as one
line. The line end (and a carriage return before it) is not part of the key;
an empty line, end of input, or a key longer than 63 characters is refused.
`add --password -` is refused, because leaving `--password` out already
means standard input.

```sh
net wifi add 'EXAMPLE-SSID'
net wifi enable
net wifi list
```

A key given with `--password` is visible to the invoking shell and may be
retained in shell history. `net` overwrites its own argv copy of the key and
its input buffer once the change has been applied, but it cannot erase copies
held by the parent shell. The interactive `net>` console does not add
`wifi add` and `wifi modify` lines to its own history.

After a successful change, `net` tells `networkd` that a store changed. The
notice carries no account, key or SSID; `networkd` takes the account from the
socket's peer credentials. If the notice cannot reach `networkd`, the change
is still saved and `net` warns `Wi-Fi network saved; networkd notification
failed`.

## Login sessions

An account's own store is used for automatic joining only while that account
has an open login session. `sessiond` runs `net wifi session open UID` after a
login and `net wifi session close UID` after the logout, as root. Without an
account operand the command is about the account that runs it.

Root may name any account in the account database. A member of the `network`
group may name only itself. At most eight sessions are listed at a time; a
further `open` fails with `ENOSPC`. Opening a session that is already listed
moves it to the newest place. The list starts empty when `networkd` starts,
so a session that stayed open across a restart of `networkd` counts again
from its next login.

Closing a session removes that account's store from the candidates. If the
account owns the policy, the policy goes back to root, the connection ends and
the search starts again. If the policy is root's but the current connection's
key came from the closing account's store, that connection ends and the
search resumes. Closing root's session ends nothing, because root's store is
the system's and stays a candidate.

## Process and authority boundary

```text
person, sessiond or desktop
        |
        v
   /sbin/net
        |
        v
root networkd ---- /sbin/wifi ---- WLAN ioctls and link state
        |
        +---------- /sbin/dhcpc --- IPv4 lease, route, resolver
        +---------- /sbin/ifconfig - link/address primitives
```

`networkd` listens on `/run/networkd.sock`, owned by `root:network` with mode
`0660`. It obtains the connecting process's effective UID from the kernel with
`SO_PEERCRED`; request fields cannot select or impersonate another account.

The enabled policy has one owner account, whose store is a candidate besides
the system's and the open sessions' stores. Root and every member of the
`network` group may control the policy whoever owns it: `enable`, `disable`,
`connect` and `disconnect` are not reserved to the owner. `enable` makes the
caller the owner. A `connect` by an account that does not own the policy moves
the policy to that account, and the key comes from that account's own store.
Any other account is refused with `EPERM`.

`net` sends no passphrase or profile record, and no interface name, in a
WLAN request. `networkd` derives each store's location from the account,
reads a selected credential only when needed, passes it to one private
`/sbin/wifi` child on descriptor 4, and wipes all operation-local copies when
the child ends. The persistent store files are the only long-lived passphrase
owners.

The `ZNV2` protocol on `/run/networkd.sock` and the private
`wifi --machine --passphrase-fd=4` form are internal implementation contracts,
not public administration interfaces.

## Credential stores and format

Each account has one store, selected from the effective UID:

| Effective UID | Store |
| --- | --- |
| `0` | `/etc/wifi.conf` (the system's store) |
| nonzero | `.wifi.conf` below that UID's passwd-record home directory |

The implementation does not trust `HOME`. `sudo net wifi add ...` therefore
updates the system's store. The store, lock, and temporary generation are
checked as single-link regular files with the selected owner and group and
mode `0600`; symlinks, replacement races, unsafe directory ownership, and
group/world-writable parent directories are rejected. Every change is one
locked rewrite: the store is read and parsed under its lock, changed, written
to a same-directory temporary file that is read back and validated, synced,
and renamed over the old one. A refused change leaves the store as it was.

The canonical version-1 file begins with `wifi-conf 1` and contains one record
per profile:

```text
wifi-conf 1
network "EXAMPLE-SSID" wpa2-personal-ccmp "REPLACE-WITH-SECRET" auto
network "MANUAL-EXAMPLE" wpa2-personal-ccmp "ANOTHER-PLACEHOLDER" manual
```

The final newline is required. SSIDs contain 1--32 decoded bytes. Passphrases
contain 8--63 printable ASCII bytes. Quoted fields accept `\"`, `\\`, `\t`,
`\n`, `\r`, and `\xHH`; embedded NUL is rejected. The file is limited to
32,768 bytes, 512 bytes per line, 64 profiles, and 4,096 decoded passphrase
bytes in total. Duplicate SSIDs, unsupported security/mode words, carriage
returns, malformed escapes, and trailing syntax are errors.

Use `net wifi add`, `modify` and `delete` instead of editing the file by hand.
`--auto yes` is stored as `auto` and `--auto no` as `manual`.

## Selection and state

There is at most one managed connection across all detected WLAN interfaces.
The policy states are:

| State | Meaning |
| --- | --- |
| `disabled` | No policy owner; managed radios are lowered |
| `auto-searching` | Policy enabled; radios scan and automatic selection may run |
| `connected` | One selected radio owns the managed L2/L3 connection |
| `manual-disconnected` | Owner retained and radios scan, but automatic selection is suppressed |

`connecting`, `reconnecting` and `disconnecting` may be visible briefly in
`net wifi list`, but are bounded operation transients rather than persistent
policy states.

`net wifi list` reports, for each discovered radio, a line of the form
`interface=NAME snapshot-age-ms=AGE scan-snapshot=available` (or
`scan-snapshot=not-yet-observed`) followed by its current scan cache, without
credentials. The last line is the policy record:

```text
wifi state=STATE interface=NAME owner=UID store=UID sessions=LIST ssid=HEX rejected=LIST scan=N
```

`interface` is `-` when no radio is selected. `owner` is the policy owner's
UID. `store` is the UID whose store gave the current connection's key, or `-`.
`sessions` lists the UIDs with an open session, oldest first, separated by
commas, or `-`. `ssid` is the current connection's SSID in lower-case hex, or
`-`. `rejected` lists remembered key refusals as `UID:SSID-IN-HEX`, or `-`.
Account numbers are not secret; keys never appear.

Automatic joining reads the candidate stores in this order, and an SSID that
an earlier store gave is not taken again:

1. the open sessions' stores, the newest session first;
2. the store of the account that owns the policy;
3. the system's store.

A store that is missing or cannot be read adds nothing; the others are still
used. Within one store, `auto` profiles are tried in file order. Selection
then chooses the first stable-discovery-order radio currently reporting a
supported visible candidate; it does not wait for every other radio's scan to
finish. Failed radios are skipped and later candidates can be considered in
subsequent sweeps. DHCP starts only after WPA2/CCMP authorization succeeds.
The selected radio alone owns managed IPv4; losing scans are stopped when
selection commits.

When a network refuses a saved key during the handshake, `networkd`
remembers the refusal (at most eight, the oldest dropped first) and automatic
joining passes over that key. A refusal is forgotten when its store changes,
when an explicit `connect` of that network with that store succeeds, and when
Wi-Fi is turned off.

`enable` first validates the caller's store and obtains a stable radio
enumeration. If the caller already owns a healthy connected policy, it keeps
that connection and its lease. Otherwise it retires the prior connection,
makes the caller the owner, brings usable radios up, and leaves association
to scheduled background work. An empty successful enumeration is a valid
`auto-searching` hotplug-wait state. If some radios fail preparation,
selection continues with the usable radios in their original stable order.

`connect SSID` requires an enabled policy and an exact saved profile in the
store it uses (the owner's, or the caller's when the caller does not own the
policy), whether that profile is `auto` or `manual`. It retires the current
connection, then waits up to 30 seconds for a radio that sees the network.
A failed explicit join leaves the policy in `manual-disconnected`: no other
saved network is joined until someone joins a network or turns Wi-Fi off and
on again.

A store change that `networkd` is told about restarts a running automatic
search when the changed store is a candidate (the system's, the owner's, or
an open session's).

`wifi wlan0 list` is a nonblocking scan-cache snapshot: an empty result while
scanning is valid, not proof that the hardware found no access points.

`dhcpc` writes first-OFFER, bound and failure events to `/dev/console`, so
background operation remains visible independently of its inherited stdout.
An OFFER is progress, not a completed lease. Inspect `net wifi list` and
interface/address state before concluding that connection has succeeded.

`disconnect` cancels managed work, retires the current managed L2/L3 state,
enters `manual-disconnected`, and leaves radios up and scanning. `disable`
retires managed state, lowers all detected WLAN radios, clears the owner and
the remembered refusals, and enters `disabled`.

On carrier loss from `connected`, `networkd` enters `reconnecting`, rereads
the store the connection's key came from, and starts exactly one ordinary
`/sbin/wifi` connection attempt for the same SSID. That command owns one
monotonic 30-second scan/select/connect window. Success returns to
`connected`; failure removes stale managed L2/L3 state and settles in
`auto-searching`. There is no nested daemon retry and no kernel-owned
high-level reconnect loop.

Startup publishes `networkd` readiness only after every detected WLAN has been
normalized through disconnect, scan stop, and interface down. Normal daemon
exit repeats that normalization. A normalization failure prevents successful
readiness or exit rather than reporting a clean `disabled` state.

## Wi-Fi across boots

The [`networking`](../../userland/base/init/services/networking) oneshot
service runs `net startup` as root at boot. It enables the wired interfaces
and then sends `enable` from a detached process, so association does not hold
up the boot. The policy enabled this way is root's, and the boot joins from the
system's store.

A successful `disable` creates the empty file `/var/db/wifi-off`; a successful
`enable` removes it. While the file exists, `net startup` prints `Wi-Fi stays
off, as it was turned off` and does not enable Wi-Fi. A failure to create or
remove the file is reported by `networkd` and otherwise ignored.

## Failure behavior

- Failure to read or validate the selected store reports a bounded
  `wifi.conf` diagnostic and does not install a new policy owner.
- `connect` while disabled fails with `Wi-Fi is disabled`; an unknown saved
  SSID (`ENOENT`), zero detected radios (`ENODEV`), a network that no radio
  sees within the selection window (`ENETUNREACH`), and a refused key
  (`EACCES`) fail distinctly.
- `enable`, `connect` and `disconnect` fail with `EBUSY` while a `disable` is
  still retiring.
- A pre-mutation validation or enumeration error preserves the previous
  policy. An error after mutation retires transaction-owned work before
  reporting failure when possible.
- Incomplete L2/L3 retirement is reported as degraded rather than successful;
  `networkd` retries it with a back-off of up to 60 seconds.
- `networkd` logs a failed operation with the stage at which it failed, never
  with a key or an SSID.
- `enable` and `connect` are bounded by a 105-second client response wait
  (90 seconds of work and a 15-second transport margin); `disable`,
  `disconnect`, `list` and `session close` by 45 seconds; the store-change
  notice and `session open` by 17 seconds. The internal `/sbin/wifi connect`
  window remains 30 seconds; selection and DHCP account for the larger
  compound-operation budget.

`/sbin/wifi` is also a privileged recovery primitive with explicit interface
forms for `up`, `down`, `search start`, `search stop`, `list`, `status`,
`connect`, and `disconnect`. Its human `connect` form places a passphrase in
argv and does not run DHCP, choose among all interfaces, read profile files, or
maintain policy. Routine administration should use `net wifi`.

## Implementation

| Contract | Production source |
| --- | --- |
| Public grammar, key input, store edits and session requests | [`userland/base/net/main.c`](../../userland/base/net/main.c) |
| Profile grammar, limits, add/modify/delete, validation, and wiping | [`wifi-conf.c`](../../userland/base/net/wifi-conf.c), [`wifi-conf.h`](../../userland/base/net/wifi-conf.h) |
| UID-derived store selection and locked atomic rewrite | [`wifi-store.c`](../../userland/base/net/wifi-store.c), [`wifi-store.h`](../../userland/base/net/wifi-store.h) |
| Authenticated socket, permissions, candidate stores, sessions, refusals, and the off record | [`networkd/main.c`](../../userland/base/networkd/main.c), [`protocol.h`](../../userland/base/net/protocol.h) |
| Managed states and link-loss recovery | [`managed-wlan.c`](../../userland/base/networkd/managed-wlan.c), [`managed-wlan-state.h`](../../userland/base/networkd/managed-wlan-state.h), [`route.h`](../../include/uapi/route.h) |
| Finite L2 primitive and secret descriptor | [`wifi/main.c`](../../userland/base/wifi/main.c), [`wifi-child.c`](../../userland/base/networkd/wifi-child.c) |
| Login and logout notices | [`sessiond/session.c`](../../userland/desktop/sessiond/session.c) |
| Boot-time `net startup` | [`networking`](../../userland/base/init/services/networking), `startup_command` in [`net/main.c`](../../userland/base/net/main.c) |
| First-OFFER, bound and failure console events | [`dhcpc/main.c`](../../userland/base/dhcpc/main.c) |

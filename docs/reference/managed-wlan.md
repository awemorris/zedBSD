# Managed WLAN administration

Status: current for the managed WLAN v1 interface

The supported high-level interface is `/sbin/net wifi`. It manages one global
WLAN policy through root `networkd`, selects saved WPA2-Personal/CCMP profiles,
and acquires IPv4 configuration only after the controlled port is authorized.
No high-level command accepts an interface operand.

The commands and configuration model build for the maintained amd64 and i386
base systems. The accepted physical radio path is the Japan-market TP-Link
Archer T3U Nano using the RTL8822BU driver, including ordinary 2.4-GHz and Japan
W52 5-GHz operation. This does not claim DFS, W53/W56, WPA3, roaming, AP mode,
or completion of the separate AX211 direct-boot work.

## Command grammar

The complete public v1 grammar is:

```text
net wifi set-key SSID PASSPHRASE [auto]
net wifi enable
net wifi disable
net wifi list
net wifi connect SSID
net wifi disconnect
```

These forms are available both as arguments to `/sbin/net` and within the
argument-free interactive `net>` console. `auto` is the only optional policy
word accepted by `set-key`. Omitting it creates a manual profile; an explicit
`manual` argument is rejected. Supplying `wlan0`, another interface name, a
UID, a file path, or a credential to any other `net wifi` operation is also a
syntax error.

For example, an administrator may save an automatically eligible profile and
enable management with:

```sh
net wifi set-key 'EXAMPLE-SSID' 'REPLACE-WITH-SECRET' auto
net wifi enable
net wifi list
```

The passphrase argument is visible to the invoking shell and may be retained
in shell history. Avoid entering a real credential in a recorded terminal,
script, bug report, or shared command log. `set-key` clears its mutable argv
copy before returning, but it cannot erase copies retained by the parent shell.

## Process and authority boundary

```text
person or desktop
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
Root and members of the `network` group may reach the administration socket,
but the active managed policy still has one authenticated owner UID. Root may
override that owner. A non-root administrator may mutate only a policy it owns,
except that `enable` may validate and install that caller as the new owner.

`net` remains stateless. It writes credentials locally only for `set-key` and
sends no passphrase, UID, home directory, profile record, or interface name in
a global WLAN request. `networkd` derives the profile location from the
attested UID, reads a selected credential only when needed, passes it to one
private `/sbin/wifi` child on descriptor 4, and wipes all operation-local
copies when the child ends. The persistent profile file is the only long-lived
passphrase owner.

The `ZNV2` protocol on `/run/networkd.sock` and the private
`wifi --machine --passphrase-fd=4` form are internal implementation contracts,
not public administration interfaces.

## Credential stores and format

`net wifi set-key` selects the store from its effective UID:

| Effective UID | Store |
| --- | --- |
| `0` | `/etc/wifi.conf` |
| nonzero | `.wifi.conf` below that UID's passwd-record home directory |

The implementation does not trust `HOME`. `sudo net wifi set-key ...`
therefore updates the root store intentionally. The store, lock, and temporary
generation are checked as single-link regular files with the selected owner and
group and mode `0600`; symlinks, replacement races, unsafe directory ownership,
and group/world-writable parent directories are rejected. Publication uses a
same-directory temporary file, validation, sync, and atomic rename. Replacing
an existing SSID preserves its profile order; a new SSID is appended.

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

Use `net wifi set-key` instead of editing the file by hand. A valid manual mode
is written by omitting `auto` from the command even though the canonical file
spells that stored mode as `manual`.

## Selection and state

There is at most one managed connection across all detected WLAN interfaces.
The persistent public policy states are:

| State | Meaning |
| --- | --- |
| `disabled` | No policy owner; managed radios are lowered |
| `auto-searching` | Policy enabled; radios scan and automatic selection may run |
| `connected` | One selected radio owns the managed L2/L3 connection |
| `manual-disconnected` | Owner retained and radios scan, but automatic selection is suppressed |

`connecting` and `reconnecting` may be visible briefly in `net wifi list`, but
are bounded operation transients rather than persistent policy states. The
first list line has the form `wifi state=STATE interface=NAME`; `-` denotes no
selected interface. The remaining bounded output reports each discovered
radio and its current scan cache without displaying credentials.

`enable` first validates the caller's fixed store and obtains a stable radio
enumeration without changing the existing policy. It then brings usable radios
up and starts asynchronous scans. An empty successful enumeration is a valid
`auto-searching` hotplug-wait state. If some radios fail preparation, selection
continues with the usable radios in their original stable order.

Automatic selection walks `auto` profiles in file order, then chooses the first
stable-discovery-order radio reporting a supported visible candidate. Manual
`connect SSID` requires an enabled policy and an exact saved profile, whether
that profile is marked manual or auto; it uses the same stable radio rule.
DHCP starts only after WPA2/CCMP authorization succeeds.

`disconnect` cancels managed work, retires the current managed L2/L3 state,
enters `manual-disconnected`, and leaves radios up and scanning. `disable`
retires managed state, lowers all detected WLAN radios, clears the owner, and
enters `disabled`.

On carrier loss from `connected`, `networkd` enters `reconnecting`, rereads the
active owner's current store, and starts exactly one ordinary `/sbin/wifi`
connection attempt for the same SSID. That command owns one monotonic 30-second
scan/select/connect retry window. Success returns to `connected`; failure
removes stale managed L2/L3 state and settles in `auto-searching`. There is no
nested daemon retry and no kernel-owned high-level reconnect loop.

Startup publishes `networkd` readiness only after every detected WLAN has been
normalized through disconnect, scan stop, and interface down. Normal daemon
exit repeats that normalization. A normalization failure prevents successful
readiness or exit rather than reporting a clean `disabled` state.

## Failure behavior

- Failure to read or validate the selected profile store reports a bounded
  `wifi.conf` diagnostic and does not install a new policy owner.
- `connect` while disabled fails with `Wi-Fi is disabled`; an unknown saved
  SSID and zero detected radios fail distinctly.
- A pre-mutation validation or enumeration error preserves the previous
  policy. An error after mutation retires transaction-owned work before
  reporting failure when possible.
- Incomplete L2/L3 retirement is reported as degraded rather than successful.
- A successful local `set-key` remains successful if the subsequent empty
  profile-change notification cannot reach `networkd`; a warning explains that
  the durable profile was saved but the daemon was not notified.
- `enable` and explicit `connect` are bounded by a 100-second client response
  wait. The internal high-level `/sbin/wifi connect` window remains 30 seconds;
  automatic selection and DHCP account for the larger compound-operation
  budget.

`/sbin/wifi` is also a privileged recovery primitive with explicit interface
forms for `up`, `down`, `search start`, `search stop`, `list`, `status`,
`connect`, and `disconnect`. Its human `connect` form places a passphrase in
argv and does not run DHCP, choose among all interfaces, read profile files, or
maintain policy. Routine administration should use `net wifi`.

## Implementation and evidence

| Contract | Production source | Executable evidence |
| --- | --- | --- |
| Public grammar and local `set-key` dispatch | [`userland/base/net/main.c`](../../userland/base/net/main.c) | [NET-T21 and NET-T30](../../plan/ws005-networking/tests/README.md) |
| Profile grammar, limits, validation, and wiping | [`wifi-conf.c`](../../userland/base/net/wifi-conf.c), [`wifi-conf.h`](../../userland/base/net/wifi-conf.h) | [wifi-conf/store runners](../../plan/ws005-networking/tests/README.md) |
| UID-derived secure publication | [`wifi-store.c`](../../userland/base/net/wifi-store.c) | [NET-T21 native and reboot evidence](../../plan/ws005-networking/phase005-wifi-credential-store/phase.md) |
| Authenticated socket and global orchestration | [`networkd/main.c`](../../userland/base/networkd/main.c), [`protocol.h`](../../userland/base/net/protocol.h) | [NET-T20, NET-T22, NET-T30](../../plan/ws005-networking/tests/README.md) |
| Managed states and link-loss recovery | [`managed-wlan.c`](../../userland/base/networkd/managed-wlan.c), [`route.h`](../../include/uapi/zedbsd/route.h) | [NET-T36](../../plan/ws005-networking/tests/README.md) |
| Finite L2 primitive and secret descriptor | [`wifi/main.c`](../../userland/base/wifi/main.c), [`wifi-child.c`](../../userland/base/networkd/wifi-child.c) | [NET-T23, NET-T24, NET-T35](../../plan/ws005-networking/tests/README.md) |

The automatic protocol, credential, orchestration, lifecycle, build, and QEMU
gates passed in q071. The project owner also accepted the consolidated
RTL8822BU physical WLAN result on 2026-09-05. See the [completed Queue
record](../../plan/queue-q071.md) and [WS005 result](../../plan/ws005-networking/ws.md).

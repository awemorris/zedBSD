# Network configuration console

Status: current for the wired configuration and confirmed-commit interface

Running `/sbin/net` without arguments opens the network configuration console.
It loads `/etc/net.conf` as the startup configuration and keeps an editable
candidate only in that `net` process. The maintained amd64 and i386 base
systems support loopback and Ethernet interfaces with one IPv4 address per
interface, DHCP, one default route, and static or DHCP resolver intent.
VLANs, bridges, and multiple addresses or default routes are not yet
applicable through this console.

## Configuration flow

Enter configuration mode, edit an interface, and commit it as follows:

```text
net> configure
net(config)> interface em0
net(config-if:em0)> static ipv4 10.0.0.101 prefix-length 16
net(config-if:em0)> exit
net(config)> commit
Commit complete.
```

The transaction commands are exactly:

```text
commit
commit confirmed MINUTES
rollback
```

`MINUTES` is required and ranges from 1 through 1440. Historical interactive
`apply`, `save`, and `discard` commands are not accepted. There is no argv
confirmed-commit form, separate `confirm` command, pending-status command,
timer extension, or timer reset.

An ordinary `commit` validates the complete candidate, reconciles running
state, and only after runtime success atomically publishes `/etc/net.conf`.
If a forward operation fails, `net` attempts a complete reconcile back to the
startup intent and leaves the persistent file unchanged.

`rollback` abandons local edits and reloads the candidate from
`/etc/net.conf`. If any administrator has an armed confirmed transaction, the
same command first asks `networkd` to execute its saved rollback immediately.

## Confirmed commit

Use a confirmed commit when a network change might sever the administration
session:

```text
net(config)> commit confirmed 5
Confirmed commit applied; rollback is armed for 5 minutes.
net(config)> commit
Commit complete.
```

The first command validates and arms a complete rollback program before it
changes running state. It does not write `/etc/net.conf`. A later ordinary
`commit` in the same living console session revalidates and reapplies the
candidate, atomically publishes it, and then disarms the transaction with the
session's opaque token.

If the originating process, terminal, or remote connection is lost, its
candidate and token cannot be reconstructed or adopted. `networkd` restores
the previous running intent when the monotonic timer expires, or another
authorized administrator can run interactive `rollback`. A networkd restart
forgets the volatile timer; because the old `/etc/net.conf` was never replaced,
the next boot still uses the last committed intent.

Only one confirmed transaction and one configuration writer may be active.
Every writer serializes through the permanent root mode-0600
`/run/net.conf.lock`. Confirming publication precedes token disarm. If all
three bounded disarm acknowledgements are lost after publication, `net` exits
nonzero and reports `outcome uncertain` instead of claiming success.

## Configuration commands

The relevant mode commands are:

```text
net> configure
net(config)> interface NAME
net(config)> show candidate
net(config)> show startup-config
net(config)> show running-config
net(config-if:NAME)> enable
net(config-if:NAME)> disable
net(config-if:NAME)> dhcp [timeout SECONDS]
net(config-if:NAME)> static ipv4 ADDRESS prefix-length BITS
net(config-if:NAME)> no ipv4
net(config-if:NAME)> mtu BYTES
net(config-if:NAME)> no mtu
```

`mtu BYTES` keeps an interface MTU of 1280 through 65535 in the candidate,
written to `/etc/net.conf` as the interface key `mtu: BYTES`; `no mtu` removes
the key, which returns the interface to its device's default MTU. The device's
own limit (1500 for most Ethernet devices, up to 9000 for those that take jumbo
frames) is applied when the value is set, so a commit with an MTU the device
refuses fails and leaves the running MTU as it was. A commit changes the MTU
while the interface stays up; a disabled interface is given none until it is
enabled. At startup the MTU is set before the interface comes up, and again
each time a cable is put back. An interface that names an MTU but neither DHCP
nor an address is not configured at startup and keeps its device's default;
`ifconfig NAME mtu BYTES` sets one by hand. An `/etc/net.conf` that carries an
`mtu` key cannot be read by a system older than this key, which then applies
none of the file.

`end` returns directly to operational mode; `exit` returns one level. Exiting
with uncommitted edits asks before discarding the process-local candidate. The
existing argv `net up`, `net down`, `net dhcp`, `net static`, `net
defaultroute`, and `net dns` recovery primitives remain available but do not
provide confirmed-commit semantics.

## Wired management commands

Besides the console, `/sbin/net` takes these argument forms for the wired
interfaces and for the boot:

```text
net lan enable
net lan disable
net lan set IF dhcp [--dns A[,B]]
net lan set IF static ADDRESS NETMASK [ROUTER] [--dns A[,B]]
net startup
```

- `net lan enable` reads `/etc/net.conf`, configures an enabled loopback
  interface itself, and hands the wired interfaces' entries to `networkd`,
  which from then on brings each one up and configures it as its cable comes
  and goes. The command returns as soon as `networkd` has recorded the policy;
  the work itself runs in the background. `networkd` never opens
  `/etc/net.conf` for this; `net` reads it and sends it.
- `net lan disable` tells `networkd` to stop managing the wired interfaces
  and leaves them as they stand.
- `net lan set` configures one existing wired interface (not the loopback and
  not a radio) with DHCP or a static IPv4 address, netmask and optional
  router, and one or two name servers with `--dns`. `networkd` checks the
  values, writes that interface's entry in `/etc/net.conf` atomically under
  the writer's lock (changing nothing else in the file but the default route
  and the name servers the command names), and applies it at once. An unknown
  interface fails with `ENODEV`, and an armed confirmed transaction makes it
  fail with `EBUSY`. If the entry is saved but cannot be applied, the command
  reports `saved, but could not be applied now`. Every outcome is logged with
  the caller's effective UID.
- `net startup` is what the boot runs (the
  [`networking`](../../userland/base/init/services/networking) oneshot
  service, as root): `net lan enable`, then the Wi-Fi `enable` from a detached
  process unless Wi-Fi was turned off (see
  [Managed WLAN administration](managed-wlan.md#wi-fi-across-boots)). When the
  `networking` service's `wait` setting in `/etc/rc.conf` is `true`, `yes` or
  `1`, it then waits up to 30 seconds for some interface to obtain an address
  and fails with `no interface was configured within 30 seconds` if none does.

Root may run all of these. A member of the `network` group may run
`net lan set`, but `net lan enable`, `net lan disable` and `net startup` are
root's.

## Implementation and evidence

| Contract | Production source |
| --- | --- |
| Console grammar and transaction ordering | [`net/main.c`](../../userland/base/net/main.c) |
| `net lan` and `net startup` | `lan_command`, `lan_set_command`, `startup_command` in [`net/main.c`](../../userland/base/net/main.c); `handle_lan_configure` in [`networkd/main.c`](../../userland/base/networkd/main.c); [`managed-lan.c`](../../userland/base/networkd/managed-lan.c), [`lan-configure.c`](../../userland/base/networkd/lan-configure.c) |
| Complete wired reconcile and rollback generation | [`reconcile.c`](../../userland/base/net/reconcile.c) |
| Atomic persistence and writer lock | [`netconf.c`](../../userland/base/net/netconf.c) |
| Volatile timer, secure program ownership, and bounded rollback | [`confirmed.c`](../../userland/base/networkd/confirmed.c), [`networkd/main.c`](../../userland/base/networkd/main.c) |
| Private ZNV2 fields and bounds | [`protocol.h`](../../userland/base/net/protocol.h) |

The private rollback language and `ZNV2` messages are implementation contracts,
not public administration interfaces.

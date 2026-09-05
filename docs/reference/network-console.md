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
```

`end` returns directly to operational mode; `exit` returns one level. Exiting
with uncommitted edits asks before discarding the process-local candidate. The
existing argv `net up`, `net down`, `net dhcp`, `net static`, `net
defaultroute`, and `net dns` recovery primitives remain available but do not
provide confirmed-commit semantics.

## Implementation and evidence

| Contract | Production source | Executable evidence |
| --- | --- | --- |
| Console grammar and transaction ordering | [`net/main.c`](../../userland/base/net/main.c) | [`confirmed-commit-test.sh`](../../plan/ws011-net-config/tests/confirmed-commit-test.sh), [`net-console-test.sh`](../../plan/ws011-net-config/tests/net-console-test.sh) |
| Complete wired reconcile and rollback generation | [`reconcile.c`](../../userland/base/net/reconcile.c) | [`netconf-reconcile-test.c`](../../plan/ws011-net-config/tests/netconf-reconcile-test.c) |
| Atomic persistence and writer lock | [`netconf.c`](../../userland/base/net/netconf.c) | [`netconf-persistence-test.c`](../../plan/ws011-net-config/tests/netconf-persistence-test.c) |
| Volatile timer, secure program ownership, and bounded rollback | [`confirmed.c`](../../userland/base/networkd/confirmed.c), [`networkd/main.c`](../../userland/base/networkd/main.c) | [`confirmed-commit-model-test.c`](../../plan/ws011-net-config/tests/confirmed-commit-model-test.c) |
| Private ZNV2 fields and bounds | [`protocol.h`](../../userland/base/net/protocol.h) | [`networkd-protocol-test.c`](../../plan/ws005-networking/tests/networkd-protocol-test.c) |

The private rollback language and `ZNV2` messages are implementation contracts,
not public administration interfaces. QEMU timeout recovery, confirmed
persistence, and the physical remote-administration scenario remain the
separate WS011 p007 acceptance phase.

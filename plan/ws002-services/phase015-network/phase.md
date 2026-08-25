# ws002-p015: initial networkd and net

WSID: `ws002`

Phase ID: `p015`

Status: complete baseline; superseded in part by `ws002-p020`

Parent WS: [WS002](../ws.md)

## Objective and design

Introduce foreground `networkd` and `/sbin/net` for loopback, interface
up/down, static IPv4, default route, initial DHCP, DNS output, and status while
retaining direct `/sbin/ifconfig` recovery.

## Acceptance and result

The first baseline completed. Synchronization, one-shot `dhcpc`, fd 3 readiness,
and command orchestration were redesigned and verified in
[`ws002-p020`](../phase020-network-service/phase.md). Further network lifecycle
and WLAN work belongs to [WS005](../../ws005-networking/ws.md).

## Interruption record

No active work remains in this Phase. Resume only through WS005 or an explicit
WS002 maintenance Phase.

## Completion conditions

- `networkd` and `net` perform the declared loopback, static IPv4, route, DNS,
  initial DHCP, and status operations.
- Managed operation and direct `/sbin/ifconfig` recovery do not conflict.
- Focused protocol/configuration and installed QEMU network cases pass.

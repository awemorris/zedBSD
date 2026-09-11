# ws011-p003: persistence and boot migration

WSID: `ws011`  
Phase ID: `p003`  
Combined ID: `ws011-p003`  
Status: complete software milestone
Parent WS: [WS011](../ws.md)

## Objective

Make `/etc/net.conf` authoritative at boot, remove network data from
`/etc/rc.conf`, and prove static and DHCP migration on amd64 NE2000 QEMU.

## Work packages

- [x] Install a valid default `net.conf` in supported base images.
- [x] Convert `net boot` from `net_*` rc.conf keys to the shared model.
- [x] Retain only service-level network settings in `rc.conf`.
- [x] Apply the supported loopback/Ethernet model with bounded errors.
- [x] Implement atomic save and interrupted-write recovery.
- [x] Update dependent planning and administrator documentation.
- [x] Test loopback boot plus static, DHCP, DNS, and route sequencing.

## Completion conditions

- No interface, address, route, DNS, or DHCP data is read from `rc.conf`.
- Invalid/missing `net.conf` reports precise failure and never silently applies
  a partial configuration.
- Valid static and NE2000 DHCP configurations reach usable login/network state
  in bounded amd64 QEMU.
- networkd FD 3 ordering and direct ifconfig recovery still pass.
- Failed saves preserve the prior valid file.

## Acceptance

Run `NPER-T001`–`NPER-T007` from the [shared test index](../tests/README.md).

## Result and remaining runtime gate

The default file is installed, QEMU boot reaches login after applying its
loopback model, and no network data remains in `rc.conf`. Focused tests prove
atomic preservation and exact static/DHCP/route/DNS networkd requests. Prior
`ws002-p020` QEMU evidence covers the same NE2000/dhcpc and direct-ifconfig
lower paths. A fresh QEMU run using a non-default migrated DHCP `net.conf`
remains desirable before claiming physical or hardware acceptance.

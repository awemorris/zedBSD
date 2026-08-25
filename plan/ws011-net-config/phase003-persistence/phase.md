# ws011-p003: persistence and boot migration

WSID: `ws011`  
Phase ID: `p003`  
Combined ID: `ws011-p003`  
Status: planned  
Parent WS: [WS011](../ws.md)

## Objective

Make `/etc/net.conf` authoritative at boot, remove network data from
`/etc/rc.conf`, and prove static and DHCP migration on amd64 NE2000 QEMU.

## Work packages

- [ ] Install a valid default `net.conf` in supported base images.
- [ ] Convert `net boot` from `net_*` rc.conf keys to the shared model.
- [ ] Retain only service-level network settings in `rc.conf`.
- [ ] Apply supported configuration in dependency order with bounded errors.
- [ ] Implement atomic save and interrupted-write recovery.
- [ ] Update dependent planning and administrator documentation.
- [ ] Test loopback, static, DHCP, DNS/route, and direct-ifconfig recovery.

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

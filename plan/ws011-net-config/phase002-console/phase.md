# ws011-p002: interactive `net` console

WSID: `ws011`  
Phase ID: `p002`  
Combined ID: `ws011-p002`  
Status: complete
Parent WS: [WS011](../ws.md)

## Objective

Make argument-free `/sbin/net` an interactive console with operational,
configuration, and interface modes backed by the `p001` model while retaining
script-safe commands.

## Initial command surface

`net>` includes `help`, `?`, `show interfaces`, `show interface NAME`,
`show routes`, `show dns`, configuration views, immediate interface operations,
`configure`, and `exit`. `interface NAME` under `net(config)>` enters
`net(config-if:NAME)>`, initially supporting enable/up/down intent, DHCP with an
optional timeout, and static IPv4.

## Work packages

- [x] Share argv dispatch and backend request helpers with interactive paths.
- [x] Add line editing and history with the existing base editing library.
- [x] Add readable mode-specific `help` and `?` output.
- [x] Implement startup, running, and candidate displays.
- [x] Implement validated candidate mutation, bounded apply, and discard.
- [x] Reject `save` honestly until the atomic persistence Phase.
- [x] Support `net dhcp ne0` and optional `--timeout=SECONDS`.
- [x] Pass NCLI-T001--T007 and the native amd64 build gate.

## Completion conditions

- `net` enters the console; EOF and `exit` leave cleanly.
- `net help` prints help and exits successfully.
- Interactive and argv forms create equivalent networkd requests.
- Invalid/ambiguous input cannot mutate candidate or running state.
- Unsaved edits follow an explicit warning/confirmation policy.

## Acceptance

Run `NCLI-T001`–`NCLI-T007` from the [shared test index](../tests/README.md).

## Apply and persistence boundary

`apply` validates the complete candidate and rejects unsupported VLAN, bridge,
multiple-address, and non-default-route objects before the first backend
request. Supported operations are then sent sequentially. A backend failure is
reported and the candidate is retained; automatic running-state rollback is
not claimed. `save` fails explicitly until `ws011-p003` supplies atomic
persistence. `discard` restores the startup snapshot.

## Evidence and result

`sh plan/ws011-net-config/tests/net-console-test.sh` reports
`WS011 net console: PASS`. `make -j16 build/amd64/bin/net` passes native compile,
link, and ELF validation. The final amd64 image build and QEMU login smoke also
pass with the new `/sbin/net` installed.

## Resume point

Execute `ws011-p003`: implement atomic `save`, make `/etc/net.conf`
authoritative at boot, and remove interface data from `rc.conf`.

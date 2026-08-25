# ws011-p002: interactive `net` console

WSID: `ws011`  
Phase ID: `p002`  
Combined ID: `ws011-p002`  
Status: planned  
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

- [ ] Share parsing/validation between interactive and argv paths.
- [ ] Add line editing and history with the existing base editing library.
- [ ] Add readable mode-specific `help` and `?` output.
- [ ] Implement startup, running, and candidate displays.
- [ ] Implement validated candidate mutation, apply, save, and discard.
- [ ] Support `net dhcp ne0` and optional `--timeout=SECONDS`.

## Completion conditions

- `net` enters the console; EOF and `exit` leave cleanly.
- `net help` prints help and exits successfully.
- Interactive and argv forms create equivalent networkd requests.
- Invalid/ambiguous input cannot mutate candidate or running state.
- Unsaved edits follow an explicit warning/confirmation policy.

## Acceptance

Run `NCLI-T001`–`NCLI-T007` from the [shared test index](../tests/README.md).

# ws011-p004: VLAN and bridge interfaces

WSID: `ws011`  
Phase ID: `p004`  
Combined ID: `ws011-p004`  
Status: Blocked by manual hold `MB-010`
Parent WS: [WS011](../ws.md)

## Objective

Implement kernel, networkd, persistence, and console support for independent
802.1Q VLAN interfaces and bridge interfaces.

## Fixed model

- A VLAN has an interface identity, one parent, and VLAN ID 1–4094; it inserts
  and removes tags and may carry L3 addresses.
- A bridge has an interface identity and members. Host L3 addresses belong to
  the bridge, not duplicate member identities.
- VLAN is not a bridge, but a VLAN interface may be a bridge member.

## Work packages

- [ ] Freeze virtual-interface creation/destruction and link-layer UAPI.
- [ ] Implement VLAN ingress/egress and parent lifecycle.
- [ ] Implement bridge learning, forwarding, aging, and member lifecycle.
- [ ] Add networkd and console operations.
- [ ] Apply topology in dependency order and reject invalid cycles.
- [ ] Add tagged-packet and bridge-forwarding QEMU tests.

## Completion conditions

- VLAN tagging, untagging, and isolation pass packet tests.
- Bridge learning and forwarding pass without incorrect member L3 ownership.
- Creation, deletion, boot restore, reconfiguration, and rollback pass.
- Physical-interface direct recovery remains available.

## Acceptance

Run `NVIR-T001`–`NVIR-T008` from the [shared test index](../tests/README.md).

## Resume point

The feature is wanted later but its detailed design is intentionally on manual
hold. Resume only after the user explicitly selects VLAN/bridge discussion;
then jointly review the virtual-interface UAPI and packet ownership with WS005
before implementation.

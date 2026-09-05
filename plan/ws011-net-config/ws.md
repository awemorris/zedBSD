# WS011: network configuration console

Last updated: 2026-09-05

WSID: `ws011`

Status: in progress; p001--p003 and p005 design are complete. VLAN/bridge is
blocked by `MB-010`; confirmed-commit implementation p006 is Ready and proposed
as the sole q073 item, while p007 owns its later consolidated acceptance.

Parent: [master plan](../master.md)

Last verified Phase: `ws011-p005` design complete

Resume point: leave p004 blocked and obtain explicit approval for proposed q073
before implementing p006. The current `/sbin/net` still implements historical
`apply`/`save`/`discard`; p006 replaces them. Extract p007 only after p006's
focused implementation gates pass.

Shared tests: [WS011 test index](tests/README.md)

## Goals

- Make argument-free `/sbin/net` enter an interactive network console while
  retaining explicit, script-safe subcommands.
- Provide `configure` and interface configuration modes without an OS-wide
  `device>`/`device#` administration mode.
- Move persistent interface, address, route, and DNS configuration from
  `/etc/rc.conf` to versioned `/etc/net.conf`.
- Model physical, loopback, VLAN, and bridge interfaces coherently and leave a
  clean extension point for WLAN.
- Preserve `/sbin/ifconfig` as a direct networkd-independent recovery path.
- Add a confirmed-commit transaction that automatically restores the prior
  network intent if remote administration is not confirmed in time.

## Objective

Provide one native interface for inspecting, applying, and persisting network
configuration. The console is Cisco-inspired at the interaction level but is a
documented zedBSD interface, not Cisco IOS compatibility. `net.conf` is a
strict YAML-like zedBSD format, not general YAML.

## Scope

- interactive operational, configuration, and interface modes;
- `net help` and the existing non-interactive commands;
- startup, running, and candidate configuration views;
- strict parsing, validation, canonical writing, atomic saving, and boot
  application of `/etc/net.conf`;
- removal of the current `net_*` data fields from `/etc/rc.conf`;
- future VLAN and bridge virtual-interface configuration and implementation.
- transaction/reconcile and confirmed-commit design before implementation.

## Non-goals

- an OS-wide configuration interpreter or Cisco IOS command compatibility;
- a complete YAML implementation;
- removal of direct `/sbin/ifconfig` ioctl control;
- WLAN authentication and euid-selected `/etc/wifi.conf` or `~/.wifi.conf`,
  which remain in WS005;
- pretending VLAN/bridge work before the kernel data paths exist.

## Cross-WS dependencies

- WS002 supplies the current init/networkd/net boot and FD 3 readiness baseline.
- WS005 owns physical networking, WLAN, WPA, and related data-path expansion.
- WS001 receives compatibility debt; WS009 owns final public documentation.

## Phase registry

| Combined ID | Phase | Status | Completion result |
| --- | --- | --- | --- |
| `ws011-p001` | [`net.conf` v1 format and parser](phase001-netconf/phase.md) | Complete | Strict native parser/model/writer and host/native build gates pass |
| `ws011-p002` | [Interactive `net` console](phase002-console/phase.md) | Complete | Three modes, candidate safety, argv sharing, help/history, and native image gates pass |
| `ws011-p003` | [Persistence and boot migration](phase003-persistence/phase.md) | Complete software milestone | Atomic authoritative configuration and boot/request evidence pass; migrated DHCP QEMU rerun remains |
| `ws011-p004` | [VLAN and bridge interfaces](phase004-vlan-bridge/phase.md) | Blocked by explicit manual hold | Resume design and implementation only after explicit user release |
| `ws011-p005` | [Confirmed-commit design](phase005-confirmed-commit-design/phase.md) | Complete design (2026-09-05) | Session-only candidate/token, networkd rollback timer, delayed config publication, and implementation bounds are frozen |
| `ws011-p006` | [Confirmed-commit implementation](phase006-confirmed-commit-implementation/phase.md) | Ready; proposed q073 | Replace `apply/save/discard` with interactive `commit`, `commit confirmed MINUTES`, and `rollback`; networkd never touches `/etc/net.conf` |
| `ws011-p007` | [Confirmed-commit acceptance](phase007-confirmed-commit-acceptance/phase.md) | Planned; follows p006 | Focused/QEMU failure and recovery matrix plus one consolidated physical remote-administration check |

## Fixed decisions

- `net` with no arguments enters `net>`; `net help` prints non-interactive help.
- Script commands remain: `show`, `up`, `down`, `dhcp`, `static`, `dns`,
  `defaultroute`, and `boot`.
- `net dhcp INTERFACE` is valid. Its configured timeout defaults to 10 seconds;
  `--timeout=SECONDS` overrides it.
- Modes are `net>`, `net(config)>`, and `net(config-if:NAME)>`; there is no
  device-management mode.
- `/etc/rc.conf` retains `networkd_enable`, `net_enable`, and service options,
  but no interface, route, DNS, or DHCP data.
- `net.conf` v1 uses two-space indentation and prohibits tabs, duplicate and
  unknown keys, anchors, aliases, tags, flow syntax, multiple documents, and
  multiline scalars. Supported scalars are bounded strings, unsigned integers,
  `true`, and `false`.
- Persistent writes use a same-directory temporary file, validation, sync, and
  atomic rename. Failure preserves the prior valid file.
- Wi-Fi secrets stay in the euid-selected `/etc/wifi.conf` or `~/.wifi.conf`
  owned by WS005 and are never duplicated in `net.conf`.
- A VLAN is a virtual interface with a parent and 802.1Q ID. A bridge is a
  separate virtual L2 interface with members; a VLAN is not a bridge.
- `commit confirmed MINUTES` follows the Junos-style user model: apply a
  candidate temporarily, then require a later confirmation before a networkd
  timer expires.
- Confirmed commit exists only in interactive configuration mode. There is no
  argv `net commit confirmed` or `net confirm` interface.
- There is no default timeout. `commit confirmed MINUTES` must state it
  explicitly; the maximum permitted value remains an implementation bound.
- `net` snapshots the old full network intent as an idempotent rollback command
  program, creates it as a 0600 `mktemp` file under `/tmp`, and asks networkd
  to open and arm that program before sending any operations for the new
  candidate.
- Failed or interrupted candidate application does not disarm rollback. Only an explicit
  ordinary `commit` in the owning interactive session does so. A networkd or
  OS restart intentionally forgets and cancels the pending timer; persistence
  across restart is not promised.
- `/etc/net.conf` remains unchanged while rollback is armed. Confirmation by
  ordinary `commit` writes the candidate for the first time. Timeout or an
  explicit `rollback` therefore restores only running state; the persistent
  file is already the old configuration.
- Only one confirmed transaction may be pending. networkd owns its runtime
  token, timeout, already-open rollback file, and execution result. The exact
  candidate exists only in the originating `net` process memory; another
  session may roll back but cannot confirm or adopt it.
- `apply`, `save`, `discard`, a separate `confirm`, timer extension, and a
  pending-status command are not in the initial final grammar.

## `net.conf` v1 model

```yaml
version: 1

interfaces:
  lo0:
    type: loopback
    enabled: true
    ipv4:
      addresses:
        - address: 127.0.0.1
          prefix-length: 8

  ne0:
    type: ethernet
    enabled: true
    ipv4:
      dhcp: true
      dhcp-timeout: 10

routes:
  - destination: default
    gateway: 10.0.0.1

dns:
  mode: dhcp
```

Future topology uses the same interface namespace:

```yaml
interfaces:
  vlan10:
    type: vlan
    enabled: true
    parent: ne0
    vlan-id: 10

  bridge0:
    type: bridge
    enabled: true
    members:
      - vlan10
      - em0
```

## Configuration states

- **startup**: the last valid `/etc/net.conf`; it is unchanged during a
  confirmed transaction.
- **running**: observed kernel/networkd state.
- **candidate**: interactive edits not yet committed.

The console provides `show startup-config`, `show running-config`, and
`show candidate`. The historical p002/p003 implementation uses
`apply`/`save`/`discard`; p005 replaces them with `commit`,
`commit confirmed MINUTES`, and `rollback`. A normal `commit` applies and then
persists. A confirmed commit applies temporarily, and its later ordinary
`commit` persists. `rollback` abandons an uncommitted candidate or immediately
executes an armed rollback program.

## Boot order

```text
init -> start networkd -> wait for FD 3 READY -> net service -> net boot
  -> parse /etc/net.conf
  -> physical/loopback links
  -> VLANs
  -> bridges and members
  -> static addresses or bounded DHCP
  -> routes
  -> DNS
```

The first three Phases apply only capabilities supported by the current
kernel. Future object types fail clearly rather than silently becoming stubs.

## WS completion conditions

WS011 is complete when the interactive and argv interfaces share one validated
model; `/etc/net.conf` is the sole persistent source for interface, address,
route, and DNS configuration; candidate/commit/confirmed-commit/rollback and
atomic failure behavior pass; static and DHCP boot pass in bounded amd64 QEMU; VLAN and bridge
interfaces operate through documented kernel/networkd paths; and direct
ifconfig recovery remains usable. Before a confirmed-commit implementation is
claimed, full-state rollback, client loss, daemon restart, timeout, and
confirmation behavior also pass separately extracted acceptance cases.

VLAN/bridge p004 is deliberately excluded from the active Queue while
`MB-010` is held; p006/p007 may complete independently without claiming p004.

## Reconsideration boundaries

Reconsider rather than force implementation if the format cannot round-trip
unambiguously, current filesystem primitives cannot provide atomic persistence,
candidate application cannot avoid unreported partial state, or VLAN/bridge
requires a breaking network-device ABI redesign.

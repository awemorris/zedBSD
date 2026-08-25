# WS011: network configuration console

Last updated: 2026-08-25

WSID: `ws011`

Status: in progress; `ws011-p001` complete

Parent: [master plan](../master.md)

Last verified Phase: `ws011-p001`

Resume point: implement `ws011-p002` using the frozen model; boot behavior is
still unchanged until `ws011-p003`.

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

## Non-goals

- an OS-wide configuration interpreter or Cisco IOS command compatibility;
- a complete YAML implementation;
- removal of direct `/sbin/ifconfig` ioctl control;
- WLAN authentication and `/etc/wpa/`, which remain in WS005;
- pretending VLAN/bridge work before the kernel data paths exist.

## Cross-WS dependencies

- WS002 supplies the current init/networkd/net boot and FD 3 readiness baseline.
- WS005 owns physical networking, WLAN, WPA, and related data-path expansion.
- WS001 receives compatibility debt; WS009 owns final public documentation.

## Phase registry

| Combined ID | Phase | Status | Completion result |
| --- | --- | --- | --- |
| `ws011-p001` | [`net.conf` v1 format and parser](phase001-netconf/phase.md) | Complete | Strict native parser/model/writer and host/native build gates pass |
| `ws011-p002` | [Interactive `net` console](phase002-console/phase.md) | Planned | Operational/configuration modes and argv commands agree |
| `ws011-p003` | [Persistence and boot migration](phase003-persistence/phase.md) | Planned | `net.conf` is authoritative and static/DHCP boot paths pass |
| `ws011-p004` | [VLAN and bridge interfaces](phase004-vlan-bridge/phase.md) | Proposed | VLAN and bridge topology operates through kernel and networkd |

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
- WPA secrets stay under `/etc/wpa/` and are never duplicated in `net.conf`.
- A VLAN is a virtual interface with a parent and 802.1Q ID. A bridge is a
  separate virtual L2 interface with members; a VLAN is not a bridge.

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

- **startup**: the last valid `/etc/net.conf`.
- **running**: observed kernel/networkd state.
- **candidate**: interactive edits not yet applied or saved.

The console provides `show startup-config`, `show running-config`, and
`show candidate`. `apply` changes running state after validation, `save` writes
the validated candidate atomically, and `discard` abandons edits. Rollback and
disconnect safety are frozen before mutation in `ws011-p002`.

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
route, and DNS configuration; candidate/apply/save/discard and atomic failure
behavior pass; static and DHCP boot pass in bounded amd64 QEMU; VLAN and bridge
interfaces operate through documented kernel/networkd paths; and direct
ifconfig recovery remains usable.

The WS may pause after `ws011-p003` with `p004` proposed, provided the master
and this registry record that boundary.

## Reconsideration boundaries

Reconsider rather than force implementation if the format cannot round-trip
unambiguously, current filesystem primitives cannot provide atomic persistence,
candidate application cannot avoid unreported partial state, or VLAN/bridge
requires a breaking network-device ABI redesign.

# WS005: networking and WPA

Last updated: 2026-08-25

WSID: `ws005`

Status: planned; Phase 20 baseline supplied by WS002

Parent: [master plan](../master.md)

Last verified Phase: none in WS005; inherited baseline is `ws002-p020`

Resume point: after WS003 hardware inventory, extract NET-01 for the first
physical network path. Do not select a WLAN backend before the controller and
firmware policy are known.

Shared tests: [WS005 test index](tests/README.md)

## Phase registry

No WS005 Phase has started. `ws002-p020` remains historical ownership of the
current `networkd`/`net` baseline; it is not renumbered into this WS.

## Goals

- Extend `networkd` and `net` from the WS002 wired baseline to real hardware and
  WLAN while preserving direct `/sbin/ifconfig` recovery.
- Use `dhcpc` as the DHCP child and a separate pluggable `wpa` child for WLAN
  authentication.
- Keep network configuration synchronous, bounded, diagnosable, and safe for
  boot use.

## WS completion conditions

WS005 is complete when wired/selected physical networking, WLAN association,
DHCP/static addressing, routes, DNS, reconnect, and failure reporting operate
through `net` → `networkd` → child backends; credential storage satisfies its
documented permissions/atomicity rules; and the direct ioctl recovery path
still works.

Existing baseline: [ws002-p020 network service](../ws002-services/phase020-network-service/phase.md)

## 1. Objective

Extend the current `init`/`networkd`/`net` design from wired static/DHCP
orchestration to physical bring-up, optional USB networking, and WLAN. Preserve
`/sbin/ifconfig` as the direct ioctl path. Keep `networkd` a small command
orchestrator and invoke a separate `dhcpc` for DHCP and a pluggable `wpa`
backend for authentication.

## 2. Stable control path

```text
/etc/rc.conf -> /sbin/net boot -> networkd
                                      +-- direct interface/route/DNS operations
                                      +-- dhcpc child for DHCP
                                      +-- wpa child for WLAN authentication

interactive /sbin/net ----------------^
```

The existing fd 3 readiness notification from `networkd` to init remains the
daemon-start synchronization mechanism. Per-interface `net` commands remain
synchronous and bounded so the boot service can distinguish ready, timeout,
and failure.

## 3. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| NET-00 | Complete with follow-ups | `networkd`, `net`, `dhcpc`, rc.conf boot orchestration, and fd 3 readiness | Phase 20 | See Phase 20 evidence and handoff list |
| NET-01 | Planned | Hardware-network bring-up procedure and diagnostics | BR-00, supported wired/USB interface | Static and DHCP paths pass on the target setup |
| NET-10 | Proposed | USB CDC Ethernet support if ECM/NCM is selected and the hardware role permits it | BR-07 CDC decision, USB support | Host interoperability, reconnect, DHCP/static, and transfer tests |
| NET-20 | Planned | Versioned `networkd`-to-`wpa` child protocol and pluggable backend contract | Process/fd primitives | Host protocol tests cover success, rejection, timeout, crash, and cancellation |
| NET-21 | Planned | `/etc/wpa/` plaintext database and safe management semantics | NET-20 | Root-only permissions, atomic update, parse/error tests, and no credential logging |
| NET-22 | Planned | `/sbin/wpa` initial backend | WLAN userspace/control ABI, NET-20/21 | Scan selection, authenticate/associate, reconnect, and useful errors on hardware |
| NET-23 | Planned | `net` WLAN commands through `networkd` and backend | NET-20–22 | End-to-end `net` operation reaches association and then DHCP/static configuration |
| NET-24 | Deferred | Additional WPA backend implementations | Stable NET-20 contract | Backend can be swapped without changing `net` or rc.conf semantics |

## 4. WPA backend contract

The initial topology is `/sbin/net` to `networkd` to an `/sbin/wpa` child.
`networkd` owns orchestration and interface lifecycle; `wpa` owns authentication
protocol details. The child communicates through dedicated stdin/stdout file
descriptors. The detailed Phase must define a versioned, bounded message format
rather than depending on human-readable prompt parsing.

The protocol must provide at least:

- startup/version negotiation and explicit readiness;
- scan request/results;
- connect request using a database entry rather than echoing a passphrase;
- association, disconnection, and error events;
- timeout/cancel and clean shutdown;
- distinction between protocol output and logs (`stderr` or syslog).

The backend executable/path is configurable internally so another
implementation can replace it without changing `/sbin/net` or rc.conf.

## 5. `/etc/wpa/` initial database

The first implementation may use plain text, but it is still a credential
store. Requirements are:

- a documented, versioned file format for SSID, authentication mode,
  passphrase/PSK reference, priority, and auto-connect policy;
- root ownership and permissions that do not expose secrets to ordinary users;
- atomic rewrite (`temporary file`, validation, sync where available, rename);
- escaping/length rules that cannot inject backend commands;
- redaction in logs, diagnostics, crash reports, and tests.

Encryption-at-rest or a secret service is a later enhancement, not a false
property of the initial plaintext format.

## 6. Boot and error semantics

For an auto-configured WLAN interface, association completes before `dhcpc` or
static address configuration. Each stage has its own timeout and error. A boot
policy may decide whether failure is fatal or allows degraded boot, but
`networkd` and `net` must report the actual stage rather than a generic failure.

The WLAN sequence is tested with host-side protocol fixtures before physical
radio testing. This validates orchestration but does not satisfy the WLAN
hardware acceptance gate.

# WS005: networking and WPA

Last updated: 2026-08-29

WSID: `ws005`

Status: active; q029 p001 automatic slice complete, physical check pending;
WLAN manually blocked

Parent: [master plan](../master.md)

Last verified result: the p001 safe DHCP/diagnostic automatic slice and
candidate-image QEMU USB-root gate pass; p001 remains incomplete until its one
physical acceptance. Physical NCM publication dependency `ws004-p018` and
deterministic hardening dependency `ws004-p020` are complete; the inherited
control-plane baseline is `ws002-p020`.

Resume point: boot the recorded p001 candidate image for one combined physical
RTL8156 carrier/static/DHCP acceptance. If that check fails, `ws004-p019` CDC
ECM/QEMU is the next Queue item before another hardware cycle. RTL8822CE
WLAN/WPA remains on `MB-006`.

Shared tests: [WS005 test index](tests/README.md)

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws005-p001`](phase001-usb-ncm-physical-datapath/phase.md) | In progress (`q029`); automatic slice complete | Carrier/deadline, clean-source, complete interface/default-route rollback, staged diagnostics, focused tests, production image, and QEMU USB-root gate pass; run one physical carrier/static/DHCP check next, retaining the first boundary on failure |

`ws002-p020` remains historical ownership of the current `networkd`/`net`
baseline; it is not renumbered into this WS.

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
/etc/net.conf -> /sbin/net boot -> networkd
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
| NET-05 | Planned in WS011 | Interactive `net`, `/etc/net.conf`, persistence, and VLAN/bridge configuration model | WS011 | WS011 owns its Phase and acceptance records |
| NET-01 | In progress (`q029` p001); automatic slice complete | Hardware-network bring-up procedure, safe DHCP transition/diagnostics, and bounded carrier/TX/RX evidence | BR-00, bound RTL8156 `ue0`, completed p020 hardening | The candidate and automatic gates pass; one combined physical static/DHCP check must pass or retain the exact failure that queues p019 next |
| NET-10 | Active; one physical hardened-NCM check precedes p019 | Independent ECM and NCM USB Ethernet implementations behind common USB/`net_device` contracts; physical NCM binding is proven and no Realtek frontend is assumed | BR-07, HW-12; HW-13 becomes next on q029 failure | p001 now tests the hardened candidate once; p019 supplies the ECM/QEMU control next only if that check fails; later work proves reconnect, DNS/external transfer, and reliability |
| NET-20 | Manually blocked (`MB-006`) | Versioned `networkd`-to-`wpa` child protocol and pluggable backend contract | Process/fd primitives, explicit release | Host protocol tests cover success, rejection, timeout, crash, and cancellation |
| NET-21 | Manually blocked (`MB-006`) | `/etc/wpa/` plaintext database and safe management semantics | NET-20, explicit release | Root-only permissions, atomic update, parse/error tests, and no credential logging |
| NET-22 | Manually blocked (`MB-006`) | `/sbin/wpa` initial RTL8822CE backend | WLAN userspace/control ABI, NET-20/21, firmware policy | Scan selection, authenticate/associate, reconnect, and useful errors on hardware |
| NET-23 | Manually blocked (`MB-006`) | `net` WLAN commands through `networkd` and backend | NET-20–22, explicit release | End-to-end `net` operation reaches association and then DHCP/static configuration |
| NET-24 | Deferred behind `MB-006` | Additional WPA backend implementations | Stable NET-20 contract | Backend can be swapped without changing `net` or `net.conf` semantics |

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
implementation can replace it without changing `/sbin/net` or `net.conf`.

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

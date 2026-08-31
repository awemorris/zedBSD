# Queue: independent CDC ECM QEMU network baseline

Last updated: 2026-09-01

QID: `q049`

Queue status: completed

Queue finished: **Yes**

Authorization: after q048's HID implementation commit, origin merge, and
push, the user explicitly directed automatic continuation to USB LAN work.
The previously optional CDC ECM baseline is therefore selected directly; it
is no longer conditional on another NCM failure.

Timebox: none. Continue through the finite implementation, focused and
regression gates, configured builds, and four-cell QEMU matrix. Defer and
record any newly discovered human judgment instead of widening this Queue.

Parent: [master plan](master.md)

Previous Queue: [q048](queue-q048.md)

## Purpose

Implement a standards-based CDC ECM class driver independently of the existing
CDC NCM driver, and prove the general USB-to-network path with QEMU's `usb-net`
device. The baseline must exercise driver-aware configuration selection,
ordinary Ethernet transfer, carrier, static IPv4, DHCP, detach/reconnect, and
shared xHCI operation with USB Storage without any VID:PID or QEMU-specific
binding rule.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p019` | [Phase](ws004-hardware/phase019-cdc-ecm-qemu-baseline/phase.md) | completed | Independent CDC ECM selects QEMU's standards configuration, publishes `ue0`, passes static and DHCP traffic plus detach/reconnect, honors the zero-packet HCD contract, and passes all four IDE/xHCI-storage QEMU cells |

## Fixed boundaries

- Keep CDC ECM and CDC NCM as independent implementations behind the existing
  USB and `net_device` interfaces. Do not introduce a speculative shared
  Ethernet backend or alter NCM wire behavior to make ECM pass.
- Match CDC class descriptors and Union association, not VID:PID, fixed port,
  fixed configuration number, or a QEMU identity.
- `DRV_USB_URB_ZERO_PACKET` is a general HCD transfer contract. Every enabled
  HCD used by ECM must honor it; do not hide an unsupported path in the ECM
  driver.
- Preserve checked URB ownership, callback drain, endpoint recovery,
  removable `net_device`, USB Storage, and shutdown contracts.
- Do not add RNDIS, physical RTL8156 work, a new public USB/network UAPI, or a
  broader network-stack redesign to this Queue.
- Use Phase-owned tests and disposable QEMU image copies. Do not consume
  `.internal/`, invoke aggregate `make check`, or change Noct source. Use
  `make -j16` for supported build gates.

## Required QEMU matrix

Each cell starts from a fresh disposable image and retains its command,
configuration, hashes, debug log, terminal result, and packet capture:

1. IDE system disk with ECM static IPv4, ARP, and ping.
2. IDE system disk with ECM DHCP and post-lease ping.
3. xHCI USB system disk plus ECM static IPv4, ARP, and ping on the same HCD.
4. xHCI USB system disk plus ECM DHCP and post-lease ping on the same HCD.

## Completion definition

q049 finishes when `ws004-p019` is `completed` or honestly `uncleared` with a
concrete resume condition. Completion requires the focused ECM fixture in
ordinary, sanitizer, and analyzer modes; explicit terminating-zero-packet HCD
evidence; existing USB/NCM/Storage/network regressions; configured amd64 and
i386 `make -j16` builds; all four QEMU cells; detach/reconnect without stale
ownership; and `git diff --check`. Physical hardware is not required and no
physical NCM result is claimed.

## Result

`ws004-p019` completed.  The focused ECM fixture passes 1,464 checks in
ordinary and sanitizer modes plus the production analyzer gate.  The general
xHCI/EHCI/UHCI terminating-zero-packet fixture and all selected USB, NCM,
Storage, network-hotplug, shutdown, configured-build, and repository-build
regressions pass.  QEMU 10.0.11 passes IDE/static, IDE/DHCP,
xHCI-USB-root/static, and xHCI-USB-root/DHCP with configuration 1 ECM
selection, `ue0`, ping, detach, and a second working generation.  No human
decision or physical checkpoint remains in this Queue.  The configured i386
kernel also passes a direct `nm -u` audit while the separate known `BUG-007`
Makefile defect remains open.

# WS005 shared test cases

Parent: [WS005](../ws.md)

| Case ID | Environment | Required observation |
| --- | --- | --- |
| NET-T00 | Regression | WS002 `networkd` fd 3 readiness, synchronous `net`, `dhcpc`, and direct-ifconfig recovery remain passing |
| NET-T10 | Physical wired/USB | Static and DHCP configuration, route/DNS output, transfer, restart, and degraded failure pass |
| NET-T20 | Host protocol | `networkd`–`wpa` version negotiation, ready, scan, connect, event, cancel, timeout, crash, and malformed records pass |
| NET-T21 | Credential DB | Permission, parsing, escaping, atomic rewrite, recovery, and secret-redaction cases pass |
| NET-T30 | WLAN fixture | `net` to `networkd` to fake `wpa` to DHCP/static orchestration passes without claiming hardware |
| NET-T31 | WLAN hardware | Scan/selection/authentication/association, DHCP/static, reconnect, and transfer pass on the selected controller |
| NET-T40 | USB CDC Ethernet | If selected, ECM/NCM interoperability, reconnect, DHCP/static, and transfer pass with device role proven |
| NET-T41 | RTL8156 first data path | After p020 and the safe automatic fixes, one final candidate gets one combined Latitude acceptance: first notification/bulk RX remain responsive, fixed-peer ARP/ping passes, and DHCP lease plus post-lease ping passes; failure retains the exact first stage and native-controller boundary |
| NET-T42 | QEMU CDC ECM control | In a separately approved Queue, QEMU `usb-net` selects ECM, publishes `ue0`, and passes carrier, static ARP/ping, DHCP, and post-lease ping in IDE-control and concurrent xHCI USB-storage topologies; physical NCM interoperability means this is no longer the automatic response to NET-T41 failure |
| NET-T43 | DHCP transition and diagnostics | Starting with no address or a static address, the old interface default is absent before DISCOVER, IPv4 source and BOOTP `ciaddr` are zero, every failed stage restores the exact prior interface/default state, success commits only the lease, and `ENETDOWN` is distinguishable from `ETIMEDOUT` |
| NET-T44 | Bound limited broadcast follow-up | With another interface's default route present, `SO_BINDTODEVICE` DHCP limited broadcast keeps destination/next-hop `255.255.255.255` on the bound interface; direct concurrent DHCP route mutation is serialized or ownership-safe before this case is claimed |

Existing executable Phase 20 and DHCP tests remain under repository `/tests`
and are cross-owned as regression inputs rather than duplicated.

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

Existing executable Phase 20 and DHCP tests remain under repository `/tests`
and are cross-owned as regression inputs rather than duplicated.


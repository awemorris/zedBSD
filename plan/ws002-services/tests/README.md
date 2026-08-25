# WS002 shared test index

Parent: [WS002](../ws.md)

Executable system tests remain under repository `/tests`; this file assigns
their planning ownership and shared acceptance role.

| Phase(s) | Test cases / executable evidence |
| --- | --- |
| `ws002-p011`–`p018` | Focused host/parser tests and component targets recorded in the legacy Phase 11–19 plan |
| `ws002-p019` | `tests/phase19-qemu-test.py`, `phase19-rc.conf`, `phase19-service`, `phase19-smoke.sh` |
| `ws002-p020` | `tests/phase20-contract-host-test.py`, `phase20-qemu-test.py`, `phase20-interactive-shell-qemu-test.py`, rc.conf/service/smoke fixtures |
| Shared DHCP/network logic | `tests/dhcp-host-test.c`, `dns-host-test.c`, `inet-stack-host-test.c`, `net-device-host-test.c`, `net-sync-host-stubs.c` |
| Shell support | `tests/sh-*-host-test.c` and the installed interactive QEMU scenario owned by `ws002-p020` |

Future WS002 maintenance Phases add cases here. WLAN, physical networking, and
expanded DHCP lifecycle cases belong primarily to WS005 and may reference the
same repository executable where ownership is stated explicitly.


# WS011 shared test cases

Parent: [WS011](../ws.md)

Formal tests and fixtures are placed here when their owning Phase starts. The
repository-wide `make check` target is not an acceptance interface.

| Case range | Owner | Required evidence |
| --- | --- | --- |
| `NCF-T001`–`NCF-T006` | `ws011-p001` | Grammar acceptance/rejection, limits, topology validation, canonical output, round trip |
| `NCLI-T001`–`NCLI-T007` | `ws011-p002` | Entry/exit/help, modes, editing, argv equivalence, candidate safety, malformed input |
| `NPER-T001`–`NPER-T007` | `ws011-p003` | Atomic persistence, rc.conf removal, boot ordering, static/DHCP QEMU, recovery |
| `NVIR-T001`–`NVIR-T008` | `ws011-p004` | VLAN tags/isolation, bridge learning/forwarding, lifecycle, cycles, rollback |

Future WLAN fixtures use synthetic, redacted credentials only.

## Executable tests

`netconf-parser-test.c` is the `ws011-p001` host contract test. Run it with:

```sh
cc -std=c11 -D_POSIX_C_SOURCE=200809L -I. -Wall -Wextra -Werror \
  userland/base/net/netconf.c \
  plan/ws011-net-config/tests/netconf-parser-test.c \
  -o /tmp/ws011-netconf-test
/tmp/ws011-netconf-test
```

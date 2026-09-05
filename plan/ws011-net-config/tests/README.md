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
| `NCOM-D001`–`NCOM-D006` | `ws011-p005` | Interactive-only confirmed commit, delayed persistence, rollback-program validation, partial failure, volatile timeout/restart, single-owner locking, and fresh DHCP reacquisition |
| `NCOM-T001`–`NCOM-T012` | `ws011-p006` | Grammar removal/addition, arm/apply/confirm, timeout, explicit rollback, session loss, stale token, concurrency, restart, bounds, partial failure, and acknowledgement loss |
| `NCOM-T020`–`NCOM-T022` | `ws011-p007` | QEMU timeout recovery, confirmed persistence, and one consolidated physical remote-administration acceptance |

Future WLAN fixtures use synthetic, redacted credentials only.

### NCOM-T001--T012 extraction

| Case | Focused implementation observation |
| --- | --- |
| `NCOM-T001` | Interactive help and parsing expose only `commit`, `commit confirmed MINUTES`, and `rollback`; removed and non-interactive forms fail without mutation |
| `NCOM-T002` | Normal commit validates and completely reconciles runtime before atomic persistence; partial forward failure restores old intent |
| `NCOM-T003` | Pre-arm accepts only a bounded root-mode-0600 regular rollback file and executes the accepted descriptor rather than a replacement pathname |
| `NCOM-T004` | The originating session temporarily applies, then ordinarily commits, atomically publishes, and disarms only its matching token with bounded retries |
| `NCOM-T005` | Monotonic timeout restores old running intent and leaves `/etc/net.conf` byte-identical |
| `NCOM-T006` | Explicit rollback executes immediately, cancels the timer, and reloads the caller's candidate from startup configuration |
| `NCOM-T007` | Client/TTY loss cannot confirm or adopt the candidate; rollback remains armed until explicit rollback or timeout |
| `NCOM-T008` | One pending transaction, `/run/net.conf.lock`, concurrent sessions, and stale-token rejection prevent replacement or cross-confirmation |
| `NCOM-T009` | Networkd restart forgets volatile state without changing `/etc/net.conf`; next boot retains the old committed intent |
| `NCOM-T010` | Malformed, truncated, symlinked, replaced-before-open, over-count, over-line, and over-total rollback programs fail before arming |
| `NCOM-T011` | Rollback continues after individual failures with bounded aggregate diagnostics; DHCP intent performs a fresh acquisition |
| `NCOM-T012` | Lost disarm acknowledgement reports nonzero `outcome uncertain`; parser, console, persistence, boot, direct ifconfig, wired, and WLAN focused regressions pass |

P006 owns host/model/protocol fixtures and supported target builds for these
cases. P007 alone owns NCOM-T020--T022 QEMU and physical acceptance.

## Executable tests

`netconf-parser-test.c` is the `ws011-p001` host contract test. Run it with:

```sh
cc -std=c11 -D_POSIX_C_SOURCE=200809L -I. -Wall -Wextra -Werror \
  userland/base/net/netconf.c \
  plan/ws011-net-config/tests/netconf-parser-test.c \
  -o /tmp/ws011-netconf-test
/tmp/ws011-netconf-test
```

`net-console-test.sh` covers NCLI-T001--T007 without requiring a live
networkd. It verifies entry/exit/help, all three prompts, candidate editing,
rejection without mutation, `net dhcp ne0` argv acceptance, and the unsaved
exit confirmation:

```sh
sh plan/ws011-net-config/tests/net-console-test.sh
make -j16 build/amd64/bin/net
```

`netconf-persistence-test.c` covers canonical atomic save, round-trip loading,
invalid-candidate preservation, and a same-operation temporary-file collision:

```sh
cc -std=c11 -D_POSIX_C_SOURCE=200809L -I. -Wall -Wextra -Werror \
  userland/base/net/netconf.c \
  plan/ws011-net-config/tests/netconf-persistence-test.c \
  -o /tmp/ws011-netconf-persistence
/tmp/ws011-netconf-persistence userland/base/etc/net.conf \
  /tmp/ws011-net.conf
```

`net-boot-test.sh` uses a bounded fake protocol peer to prove that static,
route, DNS, and DHCP models produce the intended synchronous networkd request
sequence without reading `rc.conf`:

```sh
sh plan/ws011-net-config/tests/net-boot-test.sh
```

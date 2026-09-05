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
| `NCOM-T020`–`NCOM-T021` | `ws011-p007` | Automatic QEMU timeout/client-loss recovery and confirmed persistence through reboot |
| `NCOM-T022` | `ws011-p008` | One consolidated physical remote-administration timeout and confirmation acceptance after transport/topology selection |
| `NCOM-T023` | `ws011-p009` | Target hybrid-overlay atomic-publication stage, bounded completion/error, lower-only replacement, and T021-only corrective rerun |

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
cases. P007 owns the two automatic QEMU cells NCOM-T020--T021. P008 owns only
NCOM-T022 and is not Queue-ready until its physical remote transport, target
link, trial values, and safe recovery route are selected. P009 owns the
correction extracted after q074 passed T020 but stopped inside target atomic
publication during T021; it does not rerun T020 or begin physical work.

## Executable tests

`netconf-parser-test.c` is the `ws011-p001` host contract test. Run it with:

```sh
cc -std=c11 -D_POSIX_C_SOURCE=200809L -I. -Wall -Wextra -Werror \
  -DNETCONF_LOCK_PATH='"/tmp/ws011-net.conf.lock"' \
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
  -DNETCONF_LOCK_PATH='"/tmp/ws011-net.conf.lock"' \
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

`netconf-reconcile-test.c` covers the deterministic complete-intent forward
and rollback sequences, absent-interface retirement, fresh DHCP acquisition,
and forward failure boundaries:

```sh
cc -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I. \
  -Wall -Wextra -Werror userland/base/net/netconf.c \
  userland/base/net/reconcile.c \
  plan/ws011-net-config/tests/netconf-reconcile-test.c \
  -o /tmp/ws011-netconf-reconcile
/tmp/ws011-netconf-reconcile
```

`confirmed-commit-model-test.c` covers pre-arm validation and bounds,
single-owner/token rules, monotonic expiry, accepted-descriptor execution,
partial rollback continuation, and volatile restart cleanup:

```sh
cc -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I. \
  -Wall -Wextra -Werror userland/base/networkd/confirmed.c \
  userland/base/net/protocol.c \
  plan/ws011-net-config/tests/confirmed-commit-model-test.c \
  -o /tmp/ws011-confirmed-model
/tmp/ws011-confirmed-model
```

`confirmed-commit-test.sh` drives the real interactive client against a
bounded fake ZNV2 peer. It covers normal publication ordering and partial
apply rollback, temporary apply/confirm/disarm, explicit rollback, originating
session loss, unchanged startup bytes, and lost disarm acknowledgements:

```sh
sh plan/ws011-net-config/tests/confirmed-commit-test.sh
```

## NCOM-T023 publication and stopped-guest diagnosis

The atomic-writer fixture links the real `netconf_save_atomic_locked()` and
injects open/fdopen/flush/sync/close/rename/cleanup failures. It checks exact
primary errors, old-file preservation before rename, cleanup ownership, and
canonical replacement. It uses host libc, not target overlay storage:

```sh
sh plan/ws011-net-config/tests/run-netconf-atomic-writer-host-test.sh
NCOM_SANITIZE=1 sh plan/ws011-net-config/tests/run-netconf-atomic-writer-host-test.sh
```

The overlay fixture links the real overlay sync/close/rename/publication
branches with a programmable lower/upper namespace. Its 199 checks cover
lower-only replacement and six failure stages; locks, reference management,
and storage are synthetic, so it is not UFS/FAT integration or deadlock proof:

```sh
timeout 45s make -f plan/ws011-net-config/tests/overlay-publication-host-test.mk run
timeout 45s make -f plan/ws011-net-config/tests/overlay-publication-host-test.mk sanitize
```

The FAT cost fixture links the production driver to the maintained WS018
memory-disk/VFS mocks. It compares deterministic sector-read counts for the
32 MiB, 2 KiB-cluster backing-file geometry, and rejects cycles, malformed
links and exact injected I/O errors before modifying data:

```sh
sh plan/ws011-net-config/tests/run-fat-write-cost-host-test.sh
NCOM_SANITIZE=1 sh plan/ws011-net-config/tests/run-fat-write-cost-host-test.sh
timeout 180s sh plan/ws018-kernel-architecture/tests/run-fat-native-vfs-host-test.sh
```

The companion cursor fixture checks fragmented, unaligned, and sparse writes
across FAT12/16/32 and 512/1024-byte logical sectors. It injects every backend
write failure in two multi-cluster growth sequences and verifies exact errors,
FAT/size/free-space rollback, retry, and remounted contents. Existing payload
bytes already overwritten before an error are not transactionally restored:

```sh
sh plan/ws011-net-config/tests/run-fat-write-cursor-host-test.sh
NCOM_SANITIZE=1 sh plan/ws011-net-config/tests/run-fat-write-cursor-host-test.sh
```

The QEMU runner defaults to the two original T020/T021 cells. Q075 selects
only T021. `NCOM_DIAGNOSTIC=1` enables compile-time stage markers plus debug
symbols/frame pointers and can never count as normal-build acceptance.
`NCOM_CAPTURE_FAILURE=1` instead preserves a failed guest's image, CPU/register/
stack observations, memory dump and a bounded GDB inspection window without
changing normal compilation. `NCOM_VARIANT` selects the small procedural
differences in the [p009 ten-cell matrix](../phase009-confirmed-commit-overlay-publication/phase.md).

```sh
NCOM_CELL_SELECTION=t021 NCOM_CAPTURE_FAILURE=1 NCOM_VARIANT=baseline \
  bash plan/ws011-net-config/tests/run-confirmed-commit-qemu.sh \
  plan/ws011-net-config/temp/q075-normal-01
```

Each invocation requires a new output path and executes one fresh cell. A
passing diagnostic observation is not a causal repair. Q075 reproduced the
real stop in normal case 06 and completed the causal FAT-correction path;
cases 07--10 all passed post-fix acceptance, starting with a replay of its
failed procedure. No repeat of accepted T020 was part of this matrix. See the
[q075 evidence](q075-confirmed-commit-evidence.md) for the pre/post-fix split.

`run-confirmed-commit-matrix.sh` runs the fixed ten-cell matrix sequentially,
stops at the first failure with retained evidence, and records `matrix.tsv`.
It explicitly disables diagnostic compilation and enables passive failure
capture; run it only with a Queue authorization for all ten cells.
It can resume past a host-only failure after retained-evidence revalidation,
but deliberately refuses to skip a real guest failure. The four post-fix
q075 cells are separate invocations of the single-cell runner, preserving the
original case 06 failure and its source identity; they are not a fresh ten-run
non-reproduction campaign.

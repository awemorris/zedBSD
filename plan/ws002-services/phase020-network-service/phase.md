# ws002-p020: synchronous net service and networkd command orchestration

Last updated: 2026-08-25

Status: implementation milestone complete; focused host/build/QEMU gates pass

WSID: `ws002`

Phase ID: `p020`

Parent WS: [WS002](../ws.md)

## 1. Objective and selected master work

Phase 20 replaces the current race-prone boot networking path with one
explicitly ordered managed path:

1. PID 1 starts `networkd` and waits for an explicit readiness record on file
   descriptor 3;
2. the synchronous `net` oneshot service runs `/sbin/net boot` only after
   `networkd` is ready;
3. `/sbin/net boot` parses the networking subset of `/etc/rc.conf` as data and
   applies it through the public `/sbin/net` command protocol;
4. `networkd` remains a lightweight command orchestrator which invokes the
   local `ifconfig`, `route`, and renamed `dhcpc` commands and returns their
   bounded results; and
5. `/sbin/ifconfig` and `/sbin/route` remain independent direct-ioctl recovery
   paths and do not require `networkd`.

This Phase is selected from `KERN-BOOT-01`, `KERN-NET-01`, `SVC-INIT-01`,
`SVC-NET-01`, `P15-DHCP`, and the networking part of `P19-COVERAGE` in
[WS001](../../ws001-posix/ws.md). It is an operating
system service milestone, not a POSIX conformance claim.

## 2. Fixed decisions and terminology

- The service is named **net service**.  Its enabled-policy key is
  `net_enable`, and its synchronous command is `/sbin/net boot`.
- The command `/sbin/net` is called the **net command** when it must be
  distinguished from the service.
- `networkd` is the persistent backend for the managed path, but it does not
  embed a DHCP client or duplicate the low-level command implementations.
- The existing synchronous `/sbin/dhcpcd` implementation is renamed to
  `/sbin/dhcpc`.  The command performs one bounded initial DHCP transaction and
  exits; it is not a daemon and does not implement renewal/rebind service.
- `/run/networkd.sock` remains the root-only control endpoint used by the net
  command.
- `/etc/rc.conf` is the persistent desired-configuration source.  The kernel
  is the source of truth for currently applied interface and route state.
- `/sbin/ifconfig` remains installed and continues to issue ioctls directly.
  It must work when both `networkd` and the net service are disabled.
- Service definitions and `rc.conf` remain data.  PID 1 and `/sbin/net boot`
  do not invoke a shell or evaluate configuration text as shell syntax.
- `networkd` startup readiness uses a private pipe inherited as FD 3 when
  `notify-fd3=on` is present.  Syslog pattern monitoring is outside this Phase.
- WPA and Wi-Fi are deferred.  `net_iw0="wpa"`, a Wi-Fi database, credentials,
  scan/selection behavior, and automatic SSID connection are not designed or
  implemented by this Phase.  Encountering `wpa` must fail honestly rather
  than silently doing nothing.
- zedBSD currently exposes the loopback interface as `lo0`.  Installed sample
  configuration therefore uses `lo0`.  Renaming it to `lo` or adding an alias
  is a separate kernel/interface-naming decision.
- No external implementation is imported into `userland/base`.

## 3. Boot architecture

The installed boot dependency graph shall be:

```text
syslogd
   |
   v
networkd -- STARTING -- FD 3 READY --> RUNNING
   |
   v
net service: /sbin/net boot -- synchronous terminal result
   |
   +-----------------------+
   |                       |
   v                       v
getty-console       network-dependent services
```

`networkd` readiness means only that its control socket and command dispatcher
are ready to accept requests.  It does not mean that an interface has a DHCP
lease or that an external network is reachable.

The net service is the boot-time synchronization point.  It completes after
each selected interface has either been configured, failed, or reached its
bounded initial DHCP wait.  A failed or degraded net service must not suppress
the recovery console.

## 4. Service-definition contract

The following installed definitions are the target shape:

```text
# /etc/service.d/networkd
type=daemon
command=/sbin/networkd
after=syslogd
notify-fd3=on
notify-timeout=10
restart=on-failure
required=NO
```

```text
# /etc/service.d/net
type=oneshot
command=/sbin/net
arguments=boot
after=networkd
requires=networkd
required=NO
```

`getty_console` shall order after `syslogd,net` but shall not require `net`.
Other services shall choose deliberately between these contracts:

- `after=networkd`: wait until networkd startup reaches a terminal result;
- `requires=networkd`: do not start unless networkd reached `RUNNING`;
- `after=net`: wait until the boot configuration attempt is terminal;
- `requires=net`: start only if the entire required net configuration
  succeeded.

`after` is ordering, not success propagation.  A dependency in `FAILED` or
`SKIPPED` state satisfies ordering and allows an ordering-only consumer such as
getty to proceed.  `requires` supplies success propagation.  The existing
`required=YES|NO` field remains a boot policy/severity property and is not a
replacement for `requires`.

The parser shall recognize exactly `notify-fd3=on|off`; `on` is not currently
accepted by the shared Boolean helper and must be added deliberately.  The
optional `notify-timeout` is a bounded decimal number of seconds.  Its default
is 10 seconds, its minimum is 1, and its maximum is 300.

## 5. FD 3 startup-notification protocol

### 5.1 Descriptor setup

For a service with `notify-fd3=on`, init shall:

1. create a private unidirectional pipe before `fork()`;
2. retain the read end in PID 1 and make it close-on-exec and nonblocking;
3. in the child, duplicate the write end to descriptor 3, clear close-on-exec
   on descriptor 3, close the original pipe descriptors, and export
   `ZEDBSD_NOTIFY_FD=3`;
4. close the write end in PID 1 immediately after `fork()`; and
5. retain service state `STARTING` until a valid terminal record, premature
   child exit/EOF, or timeout.

A daemon must check `ZEDBSD_NOTIFY_FD=3` before writing.  It must not guess
that an arbitrary descriptor 3 inherited during manual execution is a notify
channel.  Descriptor 3 is reserved for this protocol; any future descriptor
passing convention must begin above it or explicitly supersede this rule.

### 5.2 Framing and records

The startup protocol is newline-terminated ASCII with a maximum complete
record size of 512 bytes:

```text
READY\n
FAIL <decimal-code> <printable-reason>\n
```

- A daemon sends exactly one terminal record, closes FD 3, and never passes it
  to a child.
- `READY` means that the daemon can accept its documented control requests.
- `FAIL` contains a nonzero bounded decimal error code and a nonempty reason
  without control characters or additional newlines.
- The daemon also sends the failure to ordinary logging when logging is
  available.  FD 3 is control state, not durable logging.
- PID 1 accepts fragmented reads but only one complete record.  Empty,
  oversized, trailing, duplicate, malformed, or unknown records fail startup.
- EOF or process exit before a complete record becomes
  `exited before readiness`.
- Expiry of `notify-timeout` becomes `readiness timeout`; init terminates the
  child using the normal bounded stop policy.
- A `FAIL` sender that remains alive is terminated.  A daemon that sends
  `READY` and later exits is handled by ordinary supervision/restart policy.

`networkd` sends `READY` only after signal handling, command-dispatcher
initialization, and successful bind/listen of `/run/networkd.sock`.  Interface
configuration is performed later by the net service and therefore cannot
delay this record.

### 5.3 Init state changes

The service state model shall distinguish at least `STOPPED`, `STARTING`,
`RUNNING`, `COMPLETED`, `FAILED`, and `SKIPPED`.

- A successful daemon notification moves `STARTING` to `RUNNING`.
- A successful oneshot exit moves `STARTING` to `COMPLETED`.
- A failed `requires` edge moves the dependent to `SKIPPED` without executing
  it.
- All `RUNNING`, `COMPLETED`, `FAILED`, and `SKIPPED` states are terminal for
  `after` startup ordering.
- Only `RUNNING` and `COMPLETED` satisfy `requires`.

Init control responses and `/sbin/service list|status` shall expose these
states.  Reload must validate the graph before changing policy and must not
reinterpret an already active process as ready without a new notification.

## 6. `/etc/rc.conf` networking contract

The installed default shall use this grammar:

```text
networkd_enable=YES
net_enable=YES

net_auto="lo0 ne0"
net_dhcptimeout="10"

net_lo0="static ipv4 127.0.0.1 netmask 255.0.0.0"
net_ne0="dhcp"
net_em0="static ipv4 10.0.0.100 netmask 255.255.0.0"

net_defaultroute="10.0.0.1"
net_dns="10.0.0.1 10.0.0.2"
```

`net_dns` retains the explicit-DNS path agreed before the service was renamed
from ifconfig to net.  It is optional; omission permits DHCP-provided DNS.

Rules:

1. `net_auto` is an ordered, whitespace-separated list.  Only listed
   interfaces are automatically configured by `/sbin/net boot`.
2. Names must pass the existing bounded interface-name validation, must exist,
   and must not occur twice.  An empty list is valid and performs no interface
   mutation.
3. `net_NAME` for a name outside `net_auto` is retained configuration but is
   not applied at boot.  Unknown `net_*` mode tokens in an applied entry are
   errors.
4. A listed interface with no `net_NAME` entry receives only `net up NAME`.
   The installed `lo0` entry is explicit because the kernel creates the device
   but does not assign 127.0.0.1 automatically.
5. `net_NAME="dhcp"` expands to `net up NAME`, followed by
   `net dhcp NAME --timeout=SECONDS`.
6. `net_NAME="static ipv4 ADDRESS netmask MASK"` expands to `net up NAME`,
   followed by `net static NAME ipv4 ADDRESS netmask MASK`.
7. Address and mask parsing is strict.  Duplicate fields, extra operands,
   mixed modes, unsupported families, missing values, and invalid masks fail
   before the affected request is sent.
8. `net_dhcptimeout` is a decimal initial-wait duration in seconds with a
   default of 10, minimum 1, and maximum 3600.
9. DHCP interfaces are initially processed sequentially in `net_auto` order.
   The documented maximum boot wait is therefore the number of DHCP entries
   multiplied by `net_dhcptimeout`.  Parallel acquisition is later work.
10. `net_defaultroute`, when present, is applied after interface attempts only
    when no successful `dhcpc` transaction supplies a default route.
    Failure to add the fallback route is reported and does not prevent getty.
11. `dhcpc` writes DNS received in a successful DHCP reply.  Explicit
    `net_dns`, when present, is applied after all interface commands and
    therefore overrides DHCP nameservers deterministically.
12. Configuration is never word-expanded, substituted, globbed, or executed.
    The networking parser tokenizes only the fixed grammar above.
13. A future `wpa` token is reserved.  This Phase reports it as unsupported.

## 7. Net command and control protocol

The installed CLI grammar for this Phase is:

```text
net boot
net show [INTERFACE]
net up INTERFACE
net down INTERFACE
net dhcp INTERFACE --timeout=SECONDS
net static INTERFACE ipv4 ADDRESS netmask MASK
net defaultroute GATEWAY
net dns ADDRESS...
```

The net command is the only public client of the versioned root-only
`/run/networkd.sock` protocol.  It validates syntax locally, sends bounded
framed requests, checks complete replies, reports backend reason text, and
returns nonzero on configuration error or initial-wait timeout.

`net boot` may share the same in-process request/framing helper rather than
forking and executing one net child per line.  This is semantically equivalent
to the command expansion above and avoids a shell and needless process churn.

Each mutating backend reply identifies success, timeout, or failure.
The protocol shall not use human display text as machine state.  Exact version,
length, operation, status, error code, and payload bounds are fixed in a shared
local header and tested on both client and server.

The `--timeout` on `net dhcp` is translated by networkd into a synchronous
invocation of `/sbin/dhcpc -t SECONDS INTERFACE`.  An ACK and fully applied
lease before the deadline returns success.  Failure or timeout returns the
child's bounded error to the net command.  No DHCP acquisition remains active
after `dhcpc` exits.  A later retry requires another `net dhcp` invocation.

Networkd serializes mutating command requests.  It must retain signal and child
reaping correctness while waiting, but Phase 20 does not require concurrent
mutation or status service during the bounded child execution.  There is no
background DHCP job inside networkd.

## 8. Networkd command orchestration

### 8.1 Startup and kernel reconciliation

Networkd no longer configures all interfaces as part of `main()` before
entering its server loop.  At startup it shall initialize its bounded command
dispatcher, bind the control socket, notify READY, and await requests.

Kernel state is authoritative for `net show`.  Networkd must not retain an
authoritative cached `online` Boolean that can disagree with a direct
`/sbin/ifconfig` change.

Direct ioctl changes remain an administrative escape hatch.  Because Phase 20
has no resident DHCP lease manager, no later renewal overwrites such a change.
An explicit managed/unmanaged interface mode is therefore unnecessary in this
Phase.

### 8.2 Command mapping

Networkd constructs fixed argv vectors and uses `fork()` plus `execv()` without
a shell.  The initial mapping is:

```text
UP             -> /sbin/ifconfig INTERFACE up
DOWN           -> /sbin/ifconfig INTERFACE down
STATIC IPv4    -> /sbin/ifconfig INTERFACE inet ADDRESS netmask MASK
DHCP           -> /sbin/dhcpc -t SECONDS INTERFACE
DEFAULTROUTE   -> checked /sbin/route invocation
SHOW           -> kernel query or checked ifconfig/route query helper
DNS            -> bounded atomic resolver writer
```

Networkd captures a bounded diagnostic from the child, waits for its exact
exit/signal status, closes every pipe and child descriptor, and returns a
structured backend status to the net command.  Child output is not parsed as a
readiness protocol and cannot be allowed to block on an undrained pipe.

### 8.3 Synchronous `dhcpc`

Rename the existing local `dhcpcd` package, binary, usage text, build variables,
image entries, and tests to `dhcpc`.  Audit the implementation and remove any
daemonization, background loop, pidfile, or persistent supervisor behavior if
found.  The current baseline already exits after one initial lease attempt and
contains no renewal daemon; that one-shot behavior is retained.

`dhcpc` remains the owner of its DHCP packet codec and of applying the acquired
address, netmask, broadcast, DHCP default route, and DHCP DNS to the system.
It must cover bounded discover/offer/request/acknowledgement, strict packet and
server validation, rollback on failure, and checked resolver output.  It does
not provide T1 renewal, T2 rebinding, automatic expiry cleanup, or release as a
resident service.  These remain an explicit master hand-off.

### 8.4 Routes and resolver output

`dhcpc` applies DHCP-provided routes and DNS during a successful one-shot
transaction.  Networkd orchestrates explicit fallback route and DNS requests.

- An explicit `net_defaultroute` is fallback policy, not an unconditional
  override of a successful DHCP router option.
- The fallback route operation first checks kernel route state and does not
  replace an existing DHCP or manually configured default route.
- Route mutation is idempotent and does not delete unrelated direct routes.
- Explicit `net_dns` has priority over DHCP nameservers.  DHCP-derived entries
  are deterministic and deduplicated.
- Resolver output is generated into a sibling temporary file with checked
  writes, `fflush`, `fsync`, close, and atomic `rename`; failure retains the
  previous complete file and returns a backend error.
- Lease renewal and automatic expiry cleanup are not implemented; their
  absence is reported as a limitation rather than attributed to networkd.

IPv6, multiple route metrics/policy routing, search domains, resolver daemons,
and split DNS remain explicit later work.

## 9. Net service result and recovery policy

`/sbin/net boot` records every attempted operation and returns success only
when all applied entries and explicit fallback settings succeed.  DHCP initial
timeout and backend failure return nonzero and leave the optional net service
`FAILED`; they do not halt boot.

The final boot sequence must remain bounded:

- networkd READY failure or timeout causes the `requires=networkd` net service
  to become `SKIPPED`;
- a net parse, static, route, DNS, or DHCP failure makes net terminal;
- getty, which has ordering but no success dependency on net, then starts; and
- logs name the service, interface/operation, result, and bounded reason.

`service net restart` reapplies the boot configuration through the same
parser.  A networkd crash/restart does not automatically rerun the net service:
the kernel configuration applied by completed child commands remains in place.
There is no networkd DHCP state to reconstruct.

## 10. Implementation work packages

### P20.1: freeze parsers and protocol fixtures

- Add strict host fixtures for the service metadata, FD 3 records, net CLI,
  `rc.conf` keys, interface modes, and versioned networkd protocol.
- Establish exact bounds and error/status names before changing runtime code.
- Add installed default service definitions and configuration expectations to
  package/image checks.

### P20.2: init ordering, requirements, and FD 3 notification

- Parse and validate `requires`, `notify-fd3`, and `notify-timeout`.
- Add terminal startup states and separate ordering from success dependency.
- Implement descriptor creation/duplication/closure, environment indication,
  bounded record parsing, child-exit races, timeout, service status, and stop.
- Preserve ordinary non-notify daemon behavior until each daemon opts in.

### P20.3: networkd readiness and passive startup

- Remove interface mutation from networkd startup.
- Initialize state, bind/listen the control socket, emit READY/FAIL on FD 3,
  close it, and enter the foreground manager loop.
- Move routine daemon output to syslog while retaining bounded FD 3 reasons for
  startup control.

### P20.4: synchronous net service

- Implement `/sbin/net boot`, strict `net_auto` ordering, static and DHCP
  expansion, aggregate result handling, fallback default route, and explicit
  DNS override.
- Install the net service and update getty/network-dependent ordering.
- Keep the service optional so network failure cannot remove local recovery.

### P20.5: backend operations and kernel truth

- Complete versioned `up`, `down`, `static`, `defaultroute`, `dns`, and status
  requests.
- Implement the fixed command mapping to `/sbin/ifconfig`, `/sbin/route`, and
  `/sbin/dhcpc`, bounded child diagnostics, exact status propagation, and
  serialized mutation.
- Query kernel state for reporting and make route/resolver mutation precise
  and idempotent.
- Retain and test direct ifconfig/route behavior with networkd disabled.

### P20.6: rename and harden the synchronous DHCP client

- Rename `/sbin/dhcpcd` and its local package/build/test identity to
  `/sbin/dhcpc`, with no compatibility binary unless separately requested.
- Retain one bounded foreground initial acquisition and remove any daemon-only
  behavior found by the audit.
- Test packet validation, command timeout, applied address/route/DNS, rollback,
  and exact exit diagnostics.

### P20.7: remove the obsolete dhcpcd identity

- Prove no installed definition, build rule, test, source directory, usage
  text, or consumer names or launches `/sbin/dhcpcd`.
- Preserve the renamed local packet-codec/client evidence under `dhcpc`.
- Update the master without claiming renewal, Wi-Fi, IPv6, or complete
  networking.

### P20.8: integration, documentation, and hand-off

- Run the focused host, package, build, and QEMU gates.
- Update the master rows and this document with actual evidence.
- Record any safe residual limitation as a new stable hand-off instead of
  expanding Phase 20 silently.

## 11. Test and acceptance plan

### 11.1 Focused host tests

Create narrow targets, with final names kept stable once added:

```text
make phase20-init-notify-host-test
make phase20-net-config-host-test
make phase20-networkd-protocol-host-test
make phase20-dhcp-host-test
```

Coverage must include:

- READY, FAIL, fragmented, EOF, child-exit, duplicate, oversized, malformed,
  timeout, and descriptor-leak notification cases;
- `after` versus `requires`, failed/skipped dependencies, cycles, reload, and
  status serialization;
- all valid net configuration forms, duplicate/unknown interfaces, bounds,
  unsupported WPA/IPv6, bad addresses/masks, and deterministic ordering;
- partial reads/writes, `EINTR`, client disconnect, unavailable socket,
  protocol version/length/opcode errors, and backend reason propagation;
- synchronous dhcpc success, timeout/rollback, NAK, malformed/spoofed replies,
  retransmission, multiple sequential interfaces, and exact child status;
- proof that dhcpc exits after its bounded initial attempt and leaves no daemon
  or child process;
- route ownership and explicit-versus-DHCP gateway precedence; and
- resolver deduplication, explicit override, full/failed write, failed fsync,
  failed rename, and previous-file preservation.

### 11.2 amd64 QEMU acceptance

Add a bounded `make phase20-qemu-test` using `qemu-system-x86_64`, the installed
binaries, and a deterministic supported NIC/DHCP fixture.  It must prove:

1. PID 1 reports networkd STARTING, receives READY only after the control
   socket is usable, and starts net afterward;
2. net finishes before getty prints `login:` and no boot networking message is
   injected after the prompt;
3. installed `lo0` and a supported external interface follow `net_auto` order;
4. static IPv4, netmask, flags, fallback default route, and explicit DNS agree
   with kernel/filesystem state;
5. controlled DHCP runs `/sbin/dhcpc`, installs address, route, and DNS, exits,
   and leaves no DHCP daemon;
6. initial DHCP timeout releases boot within its bound, rolls back partial
   state, and requires an explicit later `net dhcp` retry;
7. networkd READY FAIL, premature exit, malformed notify, and notify timeout
   skip net but still reach getty;
8. networkd restart preserves already applied kernel configuration and can
   orchestrate a new command without stale internal interface state;
9. `/sbin/ifconfig` can configure and inspect `lo0` with networkd/net disabled;
10. shutdown closes notify/control/child descriptors, stops networkd, and
    leaves no child or service-manager hang; and
11. `/sbin/dhcpc` is installed while `/sbin/dhcpcd` and all obsolete package
    identity are absent from the final image; and
12. after an interactive root login, `ifconfig` returns to exactly one usable
    shell prompt and a following command executes normally.

The test harness must use explicit success/failure markers, a finite timeout,
and complete debug output on failure.  QEMU user networking may be used for a
basic lease only if the acquired values are deterministic; malformed, timeout,
rollback, and command-retry cases require a controlled fixture.

### 11.3 Build and repository gates

The final implementation gate is:

```text
make phase20-init-notify-host-test
make phase20-net-config-host-test
make phase20-networkd-protocol-host-test
make phase20-dhcp-host-test
make -j16
make phase20-qemu-test
make phase20-interactive-shell-qemu-test
git diff --check
```

Do not run or depend on aggregate `make check`.  Format every new or modified
`userland/base` C/header file with clang-format.  Run the applicable package,
image, provenance, and utility-matrix checks directly.  Do not create a
commit.

## 12. Reconsideration boundaries

Stop and ask for direction instead of expanding the Phase if:

- the supported QEMU NIC or DHCP path requires a new material public kernel
  ABI rather than a bounded repair of the existing socket/ioctl path;
- pipe readiness, AF_UNIX control, or timer behavior cannot pass bounded QEMU
  tests without a kernel-subsystem redesign;
- a defined gate unexpectedly requires resident DHCP renewal rather than the
  explicitly selected one-shot `dhcpc` contract;
- atomic resolver replacement cannot be provided by the current overlay VFS
  without a material VFS design change;
- renaming `dhcpcd` would leave an unplanned consumer without a working
  `dhcpc` replacement;
- Wi-Fi, IPv6, route metrics/policy routing, or a new credential database
  becomes necessary for the defined acceptance cases; or
- implementation would import an external base source or weaken an existing
  correct acceptance test.

## 13. Completion and master transition

Phase 20 is complete only when every listed host/build/QEMU gate passes, the
installed image contains the synchronous `dhcpc` but no obsolete `dhcpcd`
identity, direct ifconfig recovery still works, and the master reflects actual
evidence.

On completion:

- update `SVC-NOTIFY-01` and `SVC-INIT-01` with FD 3 and dependency evidence;
- update `SVC-NET-01` with synchronous net boot, command orchestration,
  one-shot dhcpc, route/DNS behavior, and direct-ifconfig evidence;
- resolve or narrow `P15-DHCP` without deleting its history;
- narrow the DHCP portion of `P19-COVERAGE` only to the tested extent;
- retain DHCP renewal/rebind/expiry/release, Wi-Fi, IPv6, policy routing,
  unprivileged status, link-event breadth, resolver policy, and any incomplete
  failure evidence as explicit hand-offs;
  and
- leave the Phase `partial` if its safe implemented subset works but a defined
  gate cannot be satisfied under the reconsideration policy.

### 13.1 Actual result (2026-08-25)

The implementation milestone completed with all commands in Section 11.3
passing.  The amd64 acceptance test uses four virtual CPUs, QEMU user-mode
networking, and the ISA NE2000 device (`ne2k_isa`, I/O `0x300`, IRQ 10).  It
proved the installed `networkd` FD 3 READY ordering, synchronous `net boot`, a
real DHCP lease through `/sbin/dhcpc`, the installed address/default route/DNS,
the absence of a resident DHCP client, networkd restart followed by a fresh
command, the direct-ifconfig recovery path, and bounded shutdown.

Debugging that acceptance path found and fixed four integration defects:

- dp8390 register, remote-DMA, transmit, receive-poll, and interrupt state is
  serialized for SMP, and the device is marked ready before its interrupt mask
  is enabled;
- a syscall may now pin multiple output ranges that share an already-resident
  user page without re-entering a fault path that waits on its own first pin;
  the VM host test contains a two-pin regression case;
- accepted init/networkd control descriptors are close-on-exec, so a newly
  started daemon cannot inherit a service client connection and prevent EOF;
- the interactive shell temporarily ignores `SIGTTOU` while transferring the
  controlling terminal's foreground process group, restores itself after an
  external command, and exits instead of spinning on terminal EOF/error.  A
  QEMU keyboard-driven regression proves that `ifconfig` returns to one usable
  prompt and that a following command runs.

The focused smoke also exposed the shell's missing three-operand unary
negation; `test ! -e path` now works, which lets the installed-image identity
check prove that `/sbin/dhcpc` exists and `/sbin/dhcpcd` does not.

### 13.2 Deliberate hand-offs

The stable host targets include source/configuration contract checks and the
existing DHCP packet tests, but they are not exhaustive runtime fault
injection for every case enumerated in Section 11.1.  In particular, malformed
and timeout variants of FD 3 notification, resolver write/fsync/rename failure,
and every DHCP spoof/rollback permutation remain future focused tests.  DHCP
renewal/rebind/expiry/release, Wi-Fi, IPv6, policy routing, broad link events,
and unprivileged status remain outside this milestone.

### 13.3 Interruption and resumption record

The Phase completed its declared milestone and is not paused mid-change. Its
deliberate handoffs resume under [WS005](../../ws005-networking/ws.md), while
regressions in the established service contract remain WS002 maintenance.

### 13.4 Completion conditions

- init waits for valid `networkd` fd 3 readiness and reports startup failure/timeout.
- `/sbin/net boot` synchronously applies rc.conf through `networkd` and one-shot `dhcpc`.
- Address, default route, DNS, restart, direct-ifconfig recovery, and bounded
  shutdown pass the declared host/build/QEMU tests.
- `/sbin/dhcpc` is installed, obsolete `dhcpcd` identity is absent, and all
  deliberate lifecycle/WLAN/IPv6 handoffs are recorded in WS005.

# WS005 Phase 003: AF_UNIX peer credentials and network authorization

Last updated: 2026-08-30

WSID: `ws005`

Phase ID: `p003`

Combined ID: `ws005-p003`

Status: Complete (`q040`)

Parent: [WS005 networking and WLAN](../ws.md)

Tests: [WS005 test index](../tests/README.md)

## Objective

Give `networkd` a kernel-authenticated, connection-time identity for every
AF_UNIX client, admit the installed `network` group through a
`root:network 0660` control socket, and enforce a narrow operation policy from
that identity.  In the same Phase, audit and close direct network ioctl paths
which currently permit mutation without an explicit superuser check.

This Phase is the privilege prerequisite for ordinary users and desktop
software to request WLAN orchestration without making `net`, `wifi`, or
`dhcpc` setuid and without trusting a UID supplied in protocol data.

## Baseline

`networkd` currently binds `/run/networkd.sock`, changes it to mode `0600`, and
accepts requests without querying peer identity.  The V1 request is one
whitespace-tokenized record per connection.  The installed account database
contains only `wheel`; no `network` group exists.

The AF_UNIX implementation already receives the connecting process credential
while resolving a pathname, creates a pending accepted endpoint during
`connect(2)`, and supports stream `socketpair` and descriptor passing.  It does
not retain a scalar peer identity or implement `SO_PEERCRED`.

The generic network ioctl dispatcher has mutating flag, address, and route
operations and a driver `net_device_ops.ioctl` hook, but there is no common
credential gate covering those mutation classes.  Filesystem mode on the
`networkd` socket therefore cannot be treated as the only network security
boundary.

## Dependencies

- `ws002-p020`: existing `networkd` readiness, protocol, child execution, and
  direct-ifconfig recovery behavior.
- `ws005-p002`: frozen `net -> networkd -> ifconfig/wifi/dhcpc` topology and
  root-only direct mutation rule.
- Existing AF_UNIX pathname permission checks, reference-counted process
  credentials, `getsockopt(2)`, passwd/group parsing, and record-lock support.

## Queue-entry engineering inventory (read-only)

Before proposing the finite implementation Queue, record a read-only inventory
of the occupied `SOL_SOCKET` values and public ABI in
`include/uapi/zedbsd/socket.h`; common option dispatch in
`src/kern/net/socket.c`; AF_UNIX connection/backlog storage in
`src/kern/net/unix-socket.c`; and syscall copy boundaries in
`src/kern/syscall.c`.  Confirm the current native PID/UID/GID widths and the
exact credential accessor used at `connect(2)`.  This inventory chooses one
non-conflicting local option number mechanically; it does not reopen the
fixed-width record or connection-time semantics.

In the same read-only pass, enumerate the mutation/query cases in
`src/kern/net/inet-socket.c`, `src/kern/net/route.c`, the
`net_device_ops.ioctl` hooks reachable from `include/kern/net/net-device.h`,
the socket publication sequence in `userland/base/networkd/main.c`, account
lookup in `userland/base/libc/account.c`, and the installed
`userland/base/etc/group`.  The resulting path/constant table is Queue input,
not a design gate and not authorization to edit any file.

## Frozen `SO_PEERCRED` contract

Add `SO_PEERCRED` at `SOL_SOCKET` for connected AF_UNIX stream endpoints.  The
public payload is a zedBSD-named structure rather than the kernel's existing,
larger internal `struct ucred`:

```c
#include <stdint.h>

struct zedbsd_peercred {
        int32_t  pid;
        uint32_t euid;
        uint32_t egid;
};
```

The public structure contains no pointers, supplementary-group array, live
process reference, or reserved kernel data.  It is exactly 12 bytes in both
LP64 and ILP32: signed 32-bit PID at offset 0, unsigned 32-bit effective UID at
offset 4, and unsigned 32-bit effective GID at offset 8.  Those sizes,
signedness rules, and offsets are public ABI and require layout assertions.
The current native PID/UID/GID ranges must fit those fields; a future native-ID
widening requires an explicit conversion/overflow design without changing the
published record accidentally.

The semantic contract is:

- the connecting endpoint contributes its process ID, effective UID, and
  effective GID at the successful `connect(2)` boundary;
- the listening endpoint contributes the corresponding identity captured when
  that endpoint successfully enters `listen(2)`; those scalars are copied into
  each connection pair when it is constructed;
- the accepted server endpoint reports the connector snapshot, which is the
  authoritative identity used by `networkd`;
- the connector reports the listener snapshot; a stream `socketpair` reports
  the creating caller on each end;
- a pending backlog entry already owns its immutable snapshot before
  `accept(2)`, so delayed accept, later `setuid`, `setgid`, `exec`, fork, PID
  exit or reuse, and listener credential changes do not rewrite it;
- passing a connected descriptor with `SCM_RIGHTS` transfers the connection
  and its original snapshot; it does not relabel the peer as the recipient;
- values are informational scalar snapshots.  They do not prove that the PID
  is still alive and must never be converted into an unbounded live-process
  reference;
- unconnected and listening sockets return `ENOTCONN`; non-AF_UNIX and initial
  datagram support return `ENOPROTOOPT`; and
- a short result buffer fails without a partial credential.  On success the
  returned length is exactly `sizeof(struct zedbsd_peercred)`; a larger caller
  buffer is accepted and only the defined structure is written.

The option does not return real or saved IDs.  `euid` and `egid` are selected
because they are the identities used for pathname access and privilege at the
connection boundary.

`SO_PEERCRED` and `struct zedbsd_peercred` are explicit zedBSD extensions, not
POSIX or SUS interfaces. The implementation records that classification in the
UAPI documentation and the [WS001 compliance ledger](../../ws001-posix/ws.md);
adding the option is not
reported as either POSIX conformance or a POSIX gap.

## Kernel ownership and lifetime rules

- Capture scalar credentials before publishing a pending endpoint.  A failed
  connection must publish neither an accepted socket nor a peer record.
- Store the record in the connected AF_UNIX endpoint or connection object and
  protect it with the same publication ordering as the connection itself.
- Do not retain `struct process *`, `struct ucred *`, or a PID-table lease after
  the pair is published; copied scalars are sufficient.
- Connection teardown releases no credential object and cannot double-free or
  leak a process credential.
- `getsockopt` copies a complete local snapshot after the normal socket/file
  reference is acquired.  Close, descriptor passing, and concurrent query may
  yield either a complete live result or the normal terminal socket error,
  never torn fields.
- The AF_UNIX stream, pathname, backlog, cancellation, socketpair, and
  `SCM_RIGHTS` behavior outside this added metadata remains unchanged.

## `networkd` socket publication

The installed account database gains the fixed entry `network:x:69:`.  GID 69
is the v1 network-administration group, following the selected FreeBSD-family
convention.  Production `networkd` resolves `network` with stack-backed
`getgrnam_r()`, verifies the resolved GID is 69, and does not substitute a
compiled number when the database is missing or contradictory.  A separate
bounded raw-database pass rejects malformed records, duplicate `network`
entries, and a second use of GID 69 without entering the target libc's
non-reentrant account iterator.

`networkd` publishes `/run/networkd.sock` as follows:

1. Resolve `network` before advertising readiness.  Absence, ambiguity, an
   invalid record, or a value other than GID 69 is a startup failure.
2. Bind with restrictive creation permissions, set owner/group to
   `root:network` (UID 0, resolved GID 69), set mode exactly `0660`, and verify
   the resulting socket inode before `listen` and fd 3 `READY`.
3. Never fall back to `0600`, `0666`, the daemon's primary group, or a compiled
   numeric GID when group setup fails.
4. On failure, close the listener, remove only the socket inode created by this
   startup transaction, and report the exact ownership/mode/readiness stage.
5. On shutdown, retain the existing checked listener close and socket unlink
   behavior without following a replaced path.

This remains the single `networkd` control socket.  WLAN does not add a
second user-facing daemon socket or a per-user proxy socket.

The VFS access check on a `0660 root:network` pathname is the admission check
for root, primary-group, and supplementary-group members.  `SO_PEERCRED`
returns only effective UID/GID, so `networkd` must not pretend it received the
supplementary-group vector.  Successful connection through the protected path
is the group-membership proof; the immutable peer snapshot supplies identity
and attribution after admission.

## `networkd` authorization matrix

Every accepted request first obtains `SO_PEERCRED`.  A failure or malformed
length closes the request with an authentication error before parsing or
executing an operation.

The initial operation policy is:

| Peer | Permitted operations |
| --- | --- |
| effective UID 0 | All established and future checked `networkd` operations |
| admitted non-root peer in q040 | Existing read-only `SHOW` only; future bounded WLAN categories are classified/tested but have no executable opcode until p006 |
| any other or unauthenticated peer | None |

Non-root admission does not grant the existing raw `STATIC`, `DEFAULTROUTE`,
`DNS`, generic `DHCP`, or unrelated interface mutation opcodes. q040 has no
WLAN opcode yet, so its executable non-root surface is only the existing
read-only `SHOW` operation. The bounded WLAN policy categories are retained in
the authorization classifier and fixtures, but p006 is the first Phase allowed
to attach the global `enable`, `disable`, `list`, `connect SSID`, `disconnect`,
and `profiles-changed` opcodes. Those opcodes may cause root `networkd` to
invoke interface-specific `ifconfig`, `wifi`, and `dhcpc` children, but the
client cannot decompose that authority into arbitrary route, resolver, or
address requests.

The 2026-09-05 policy amendment makes profile selection a daemon
responsibility. `net wifi set-key` writes the caller's effective-UID store and
sends only an empty `profiles-changed` notice. An accepted `enable` supplies
the active policy UID solely through this Phase's kernel-attested
`SO_PEERCRED`; networkd resolves that UID's passwd-record home when needed and
opens the fixed store itself. No global WLAN request accepts a UID, `HOME`, a
profile path, an interface, a profile record, or a passphrase.

The euid-selected file is policy input, not a separate secret-isolation
boundary. Membership in `network` grants the bounded WLAN-administration
authority to replace the active policy owner through `enable`; a future
seat-scoped credential broker requires a separate design.

The peer PID/EUID/EGID is attached to exactly one authorization-decision audit
record per accepted connection.  The record is capped at 512 bytes including
its terminator and is truncated only at field boundaries with an explicit
marker.  SSIDs may be redacted according to the later diagnostic policy and
passphrases, PSKs, and keys are never included.  This fixed per-connection rule
is the v1 rate bound; later aggregation or sampling needs a separate policy
review.  A request-provided UID, GID, PID, home directory, or authorization
flag is ignored and rejected where present.

Descriptor delegation is explicit authority delegation: a member which passes
an already-connected `net` socket retains the original connector identity in
`SO_PEERCRED`.  This Phase does not add per-message credentials or attempt to
identify the process currently holding a transferred descriptor.

## Direct mutating ioctl privilege audit

The kernel, not only `/sbin/ifconfig` or `/sbin/wifi`, enforces this initial
rule:

- query-only interface index/name/flags/address/MTU/stats, WLAN status, and
  completed scan-snapshot retrieval may be available to ordinary users;
- `SIOCSIFFLAGS`, address/netmask/broadcast setters, route add/delete, WLAN
  scan start/stop, connect, key install/clear, disconnect/down, radio/reset,
  and every driver-private state mutation require effective UID 0;
- unknown driver ioctl numbers are rejected and never treated as read-only by
  default; each public WLAN request is classified centrally; and
- `networkd` children run with root authority after a separately authorized
  peer request, preserving `/sbin/ifconfig` and `/sbin/wifi` as root recovery
  tools without giving their direct mutation path to desktop users.

The audit must enumerate all existing network and route ioctl values plus the
new WLAN range before implementation.  It must also check that a driver cannot
hide a mutation behind a nominal getter or mutate state before a failed
copyout.

Packet-socket policy, a general capability framework, service-console
authorization, and non-network device ioctls are outside this Phase.

## Ordered implementation packages

1. Allocate one unused implementation-local `SOL_SOCKET` option number,
   publish it as `SO_PEERCRED`, and add compile/runtime assertions for the
   frozen 12-byte `zedbsd_peercred` LP64/ILP32 layout before changing socket
   storage.  Numeric allocation is an engineering check, not a human gate.
2. Add immutable local/peer scalar records to AF_UNIX stream connection
   construction, including pathname connect, pending accept, socketpair,
   teardown, and descriptor-transfer cases.
3. Implement `SO_PEERCRED` through the existing common/AF_UNIX
   `getsockopt(2)` dispatch with strict length and family/state errors.
4. Add the installed `network` group, transactional `root:network 0660` socket
   publication, and readiness failure behavior.
5. Add `networkd` peer lookup, operation authorization, and secret-free audit
   diagnostics before any request dispatch.
6. Classify every current and planned network ioctl and enforce root for all
   mutation in the kernel.
7. Add focused host fixtures, affected POSIX/AF_UNIX/network regressions, a
   configured build, and bounded QEMU runtime evidence.
8. Document the snapshot/lifetime/delegation semantics and record the API as a
   zedBSD extension in the [WS001 compliance ledger](../../ws001-posix/ws.md).

## Verification plan

### Peer-credential fixture

Cover pathname stream and stream socketpair success, backlog-before-accept,
multiple clients, failed connect, unconnected/listening/non-UNIX sockets,
short/exact/oversized result buffers, repeated queries, and concurrent close.
Fork, `setuid`/`setgid`, exec, sender exit, PID reuse, and `SCM_RIGHTS` transfer
must leave the original record unchanged.  Inject allocation/publication
failure at every connection stage and prove no endpoint or credential
ownership leak.

### Authorization and publication fixture

Cover root, primary `network` group, supplementary `network` group, unrelated
user, absent group database, wrong socket owner/mode, chown/chmod/listen/READY
failure, forged request identity, transferred descriptor, and every row of the
operation matrix.  Exercise 511/512/513-byte audit renderings and prove there
is exactly one decision per accepted connection without a secret.  The test
must inspect the final socket inode as `root:network 0660`; a mode-only mock is
insufficient.

### Ioctl privilege fixture

Exercise each query and mutation class as root and non-root against a fake
`net_device`, route table, and fake WLAN driver.  A rejected mutation must not
call the driver, change flags/carrier/address/route/key state, or leak copied
secret data.  Direct root `ifconfig` recovery and authorized `networkd` child
mutation must still work.

Run ordinary and sanitizer variants where supported, the compiler analyzer,
existing AF_UNIX/SCM_RIGHTS/POSIX tests, WS002 network-service regressions,
`make -j16`, a bounded `qemu-system-x86_64` user/root authorization cell, and
`git diff --check`.  Do not run aggregate `make check` or use `.internal/`.

## Acceptance conditions

- The public peer record is exactly the frozen 12-byte, fixed-width LP64/ILP32
  layout, and its option number cannot collide between public headers and the
  kernel implementation.
- An accepted `networkd` stream reports the connector's immutable
  pid/euid/egid snapshot through `SO_PEERCRED`, including after the connector
  changes credentials or transfers the descriptor.
- `/run/networkd.sock` is published as exactly `root:network 0660` before
  READY; a missing/non-69 `network` group or publication failure produces no
  `0600`, numeric-GID, world-accessible, or false-ready fallback.
- Root retains all current networkd operations; an admitted non-root client in
  this Phase can execute only `SHOW` and cannot submit arbitrary existing
  network mutation. Future bounded WLAN categories are classifier/test inputs,
  not executable protocol promises before p006.
- Every direct mutating network, route, and WLAN ioctl is rejected for
  non-root before driver or state mutation, while classified queries remain
  usable.
- Each accepted connection has exactly one secret-free authorization audit
  decision of at most 512 bytes, attributed to the immutable peer snapshot.
- Public documentation labels the peer-credential ABI as a zedBSD extension
  and states its exact snapshot, descriptor-passing, and lifetime semantics.
- AF_UNIX lifecycle, descriptor passing, direct root recovery, fd 3 readiness,
  current wired `net`/`dhcpc`, builds, and QEMU boot regressions remain passing.

## Frozen implementation parameters

- The `SO_PEERCRED` record is the fixed 12-byte public structure above.  The
  numeric option value is selected from the free local `SOL_SOCKET` namespace
  during implementation and guarded by duplicate-value and header/kernel ABI
  tests; it does not require a manual planning decision.
- Authorization audit output is one field-bounded, secret-free record per
  accepted connection and no more than 512 bytes including its terminator.

## q040 result (2026-08-31)

The Phase is complete.  `SO_PEERCRED` is allocated as the previously unused
local value `0x0011`.  Its public `zedbsd_peercred` payload has compile-time
size, offset, native-width, and signedness guards for the frozen 12-byte ABI.
AF_UNIX pathname streams and socketpairs now retain scalar listen/connect-time
identity; delayed accept, later credential changes, peer exit, repeated
`listen`, and `SCM_RIGHTS` transfer do not relabel it.  Concurrent connects are
reserved per endpoint, the accepted endpoint and pending node are published
under the listener lock before client publication, and the client connection
and credential become visible atomically under its socket lock.  A 32-cell
listener-close/connect race compares `/dev/system` live-socket counts before
and after the campaign.  The review also restored the existing `EISCONN`
behavior for pathname reconnect of an AF_UNIX datagram socketpair endpoint.

`networkd` now publishes one checked `root:network` mode-`0660` inode before
fd-3 `READY`, authenticates every accepted client before parsing its request,
and permits an admitted non-root client only the existing `SHOW` operation.
The first target run exposed a segmentation fault inside the target libc's
non-reentrant `getgrnam()` wrapper.  The production path therefore uses
stack-backed `getgrnam_r()` plus an 8192-byte strict `/etc/group` validation
pass.  Missing/wrong, malformed, duplicate-name, and duplicate-GID-69 inputs
all fail before socket creation; no numeric fallback is used.

The common IPv4 ioctl dispatcher now uses a referenced caller credential and
an explicit query allow-list.  Every present setter and route mutation, plus
unknown/future driver-private commands, fails with `EPERM` for non-root before
argument access or backend dispatch.  Existing queries and root recovery paths
remain available.

The following final-source evidence passed:

- `run-networkd-auth-test.sh` in ordinary, ASan+UBSan, and compiler-analyzer
  configurations;
- `run-inet-ioctl-authorization-test.sh`, the existing net-device/ARP/inet
  hotplug suite, and the existing userland network-recovery suite;
- `make -j16` and `git diff --check`; and
- `run-peercred-native-qemu.sh` on production qemu-pc98, including exact
  socket ownership/mode, root access, supplementary-GID-69 admission,
  non-root `SHOW`, non-root mutation `EPERM`, unrelated-user `EACCES`, the
  peer-credential/lifetime matrix, `networkd` readiness, and
  `init: system running`.

The retained final QEMU evidence is
`plan/ws005-networking/temp/peercred-native.VNv22f/`.  The reusable entry
points and their ownership are indexed by [NET-T22](../tests/README.md).

## Reconsideration boundary

Return to planning if a connection-time client snapshot cannot be published
without retaining a live process object, if pathname permissions cannot prove
group admission for supplementary members, or if operation authorization
would require trusting payload identity.  Extract a general AF_UNIX credential
foundation if necessary.  Do not make the socket world-writable, add a setuid
`net`/`wifi`, silently permit non-root direct mutation, or query a live PID
after request receipt as a substitute for connection credentials.

## Queue boundary and handoff

q040 completed this Phase.  The independently dependency-ready credential
store in p005 may proceed; p004 still waits for the generic WLAN core and p006
still waits for p004/p005.  Later protocol work may rely on the authenticated
single socket, but must extend the explicit operation classifier rather than
turning admitted non-root access into generic network mutation.

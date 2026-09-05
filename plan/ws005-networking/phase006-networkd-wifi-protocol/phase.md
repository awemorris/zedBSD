# WS005 Phase 006: networkd Wi-Fi control protocol

Last updated: 2026-09-05

Phase ID: `ws005-p006`

Status: in progress in q071; focused protocol/auth/store/child gates pass;
combined physical acceptance remains in p008

Parent: [WS005](../ws.md)

Tests: [WS005 test index](../tests/README.md)

Architecture: [WLAN v1 control contract](../phase002-wlan-v1-contract/phase.md)

Authentication: [AF_UNIX peer credentials](../phase003-unix-peer-credentials/phase.md)

Primitive: [wifi ioctl command](../phase004-wifi-ioctl-command/phase.md)

Profiles: [Wi-Fi credential store](../phase005-wifi-credential-store/phase.md)

## Objective

Replace the obsolete `networkd`-to-`wpa` child topology with one bounded,
credential-aware control path:

```text
user or desktop -> /sbin/net -> /run/networkd.sock -> networkd
                                                    +-- /sbin/ifconfig
                                                    +-- /sbin/wifi
                                                    `-- /sbin/dhcpc
```

Replace every current networkd request with the length-framed `ZNV2`
request/response protocol on the existing `/run/networkd.sock`, authenticate
every accepted client with the p003 AF_UNIX peer-credential snapshot, and
provide a bounded child runner which passes a Wi-Fi secret to `/sbin/wifi`
through a dedicated descriptor without placing it in child argv, diagnostics,
or machine-readable output.

The WLAN request surface is global: `enable`, `disable`, `list`, `connect
SSID`, `disconnect`, and the nonsecret `profiles-changed` notification. It has
no public interface operand. `net` sends neither a credential nor a profile
path. Networkd obtains the active policy UID from the authenticated `enable`
peer, discovers `wlanN` interfaces, reads the fixed p005 store for that UID
when needed, and wipes a passphrase immediately after the one child operation.
`set-key` itself is local to `net`; its optional literal is only `auto`, and an
omitted option means manual rather than sending a `manual` protocol value.

This Phase establishes transport, authorization, subprocess, timeout, and
cancellation contracts. The public `net wifi` operation sequence and its
cross-stage rollback belong to `ws005-p007`; the later resident managed-link
policy belongs to `ws005-p011` and must consume, not bypass, these contracts.

## Baseline and motivating findings

- The current listener is one AF_UNIX stream socket at `/run/networkd.sock`,
  already published by p003 as `root:network` mode `0660`. `net` opens one
  connection, writes one legacy literal `V1`
  (planning name `ZNV1`) line, shuts down its write side, reads one response,
  and closes it.
- Legacy V1/ZNV1 is newline framed and `networkd` splits it with
  `strtok(" \t")`.
  Therefore it cannot preserve an SSID containing whitespace and must never be
  extended by placing a passphrase in an operand.
- The p003 contract supplies a fixed-width AF_UNIX `SO_PEERCRED`-style snapshot
  and its automatic tests. This Phase consumes that contract; it does not
  implement a second peer-credential API.
- `networkd` executes absolute child paths without a shell, which is retained.
  Its current runner merges stdout and stderr into one temporary file, reads
  only a short diagnostic after exit, does not bound bytes written by the
  child, and can only pass argv.
- `networkd` handles one accepted request at a time. This supplies global
  serialization today but also means an unbounded scan, association, or DHCP
  child stalls all network control.
- The earlier WS005 plan expected a separately versioned `/sbin/wpa` protocol
  and `/etc/wpa/`. That process and protocol do not exist in production, and
  the selected architecture now assigns primitive WLAN control to `/sbin/wifi`
  and persistence to `wifi.conf`.

## Dependencies

- The existing WS002 `net`/`networkd`/`dhcpc` baseline and fd-3 readiness
  contract.
- [`ws005-p003`](../phase003-unix-peer-credentials/phase.md) AF_UNIX
  peer-credential UAPI, connect-time snapshot, `getsockopt`
  implementation, and lifecycle/authorization tests.
- [`ws004-p044`](../../ws004-hardware/phase044-wlan-async-operation-boundary/phase.md)
  supplies prompt asynchronous scan and single-attempt connect generations;
  no WLAN ioctl owns the userspace command's 30-second deadline.
- [`ws005-p004`](../phase004-wifi-ioctl-command/phase.md) supplies the minimum
  human direct-root `/sbin/wifi`. This Phase extends that same binary with
  `WIFI1` machine records and the private passphrase-descriptor form.
- [`ws005-p005`](../phase005-wifi-credential-store/phase.md) versioned
  `/etc/wifi.conf` and per-user `~/.wifi.conf` parser/store used by local
  `set-key` and the networkd policy reader, including explicit-length
  SSID/passphrase fields and secret wiping.
- [`ws005-p009`](../phase009-wlan-minimum-connectivity/phase.md) first proves
  that the direct primitive and physical L2/DHCP data path work. p006 then adds
  machine framing and daemon composition without making those mechanisms a
  prerequisite for first communication.
- [`ws005-p010`](../phase010-wifi-primitive-hardening/phase.md) completes the
  human command's sole userspace 30-second retry before p006 exposes that same
  behavior through `WIFI1` and fd 4.
- AF_UNIX descriptor passing remains available for tests and future protocol
  evolution, but this Phase does not create a second daemon socket.

## Scope

- `ZNV2` outer and inner framing, size limits, strict parsing, and errors;
- migration of the current wired operations and new Wi-Fi operations to ZNV2,
  with no legacy V1 execution path at Phase completion;
- consumption and regression coverage of the p003 AF_UNIX peer-credential
  UAPI and accepted-socket snapshot semantics;
- root versus ordinary-user authorization at the networkd request boundary;
- one fixed `root:network` mode-`0660` listener policy;
- global WLAN opcodes with no public interface field, and an empty/nonsecret
  `profiles-changed` notification;
- active policy UID capture from the authenticated `enable` peer and checked
  daemon-side reads of that UID's fixed p005 store;
- a bounded `/sbin/wifi` child protocol with a dedicated secret descriptor;
- separate machine output and sanitized diagnostics;
- total child deadlines, termination, reaping, and operation cancellation;
- host/native fixtures for framing, credentials, output bounds, timeout,
  cancellation, crash, and redaction;
- fd-3 readiness and ZNV2 wired-operation regressions.

## Non-goals

- implementing or changing the public `net wifi` orchestration sequence;
- putting DHCP into `/sbin/wifi`;
- reviving a persistent `/sbin/wpa` process or adding a second control socket;
- defining WPA/RSN cryptography inside `networkd`;
- accepting a UID, home, profile pathname, passphrase, or profile record from
  a ZNV2 WLAN request;
- changing `/etc/net.conf` or confirmed-commit semantics;
- claiming Archer hardware success.

## Fixed topology and compatibility rules

1. `/run/networkd.sock` remains the only networkd control socket. There is no
   `/run/wpa.sock`, `/run/wifi.sock`, or per-user daemon.
2. One client connection carries exactly one request and one response. A client
   must half-close or close after the complete request; trailing frames or
   bytes are rejected.
3. Wired and Wi-Fi callers move to `ZNV2` in the same change. The V1 parser and
   executor are deleted rather than retained for a transition window. A
   complete legacy `V1` magic can receive only a bounded unsupported-version
   error from initial protocol detection and never executes an operation.
4. Protocol detection is by the complete initial magic, not a permissive
   prefix. A malformed, legacy, or mixed connection fails without executing an
   operation or parsing a secret as a command.
5. Every accepted connection obtains peer credentials before request dispatch.
   Authentication is not inferred from socket ownership, a claimed uid, the
   environment, or a pathname supplied in the request.
6. `networkd` continues to use absolute executable paths and `execv`; it never
   invokes a shell.

## ZNV2 framing contract

The outer frame is an ASCII version/request header followed by an exact byte
payload:

```text
ZNV2 <decimal-request-id> <decimal-opcode> <decimal-payload-length>\n
<exactly decimal-payload-length bytes>
```

The response uses the same framing and echoes the request ID and opcode; its
payload contains exactly one typed terminal status. The header is at most 32
bytes, a request payload is at most 4096 bytes, and a response payload is at
most 32768 bytes. The following parsing rules are fixed:

- the header has a small independent maximum and must end in one `\n`;
- request ID and opcode are bounded unsigned integers and payload length is a
  bounded size. CR, signs, leading/trailing/repeated whitespace, an empty
  field, integer overflow, non-canonical decimal, and a payload over the
  configured maximum are rejected;
- premature EOF, extra payload bytes, a second frame, and a client which does
  not finish before the read deadline are rejected;
- request and response maximums are independent; a scan list has at most 64
  records and still fits the 32768-byte response ceiling;
- the inner message carries fixed-width flags/status and explicitly
  length-delimited fields. It does not depend on NUL termination, host pointer
  width, native struct padding, locale, or `strtok`;
- exactly one terminal response echoes the request ID and opcode. Any operation
  state is a typed bounded field in that response, not an unsolicited second
  frame;
- unknown opcode, flag, field, enum, nonzero reserved value, duplicate field,
  or length mismatch is an explicit protocol error;
- SSID bytes are bounded by the WLAN UAPI maximum and may contain whitespace.
  NUL/non-text handling follows the lower-layer SSID contract rather than C
  string rules;
- WLAN opcodes are `enable`, `disable`, `list`, `connect`, `disconnect`, and
  `profiles-changed`. Only `connect` carries an SSID. The others have empty
  request payloads; no WLAN request carries an interface, UID, home, pathname,
  passphrase, or profile record;
- `profiles-changed` is a five-second-total, best-effort notification after a
  successful local store update. Its failure is nonsecret and cannot roll back
  or falsify that update;
- errors identify the stage and stable error number but never echo a complete
  request, SSID unless safely escaped, passphrase, PMK, or child argv.

The exact numeric opcode/field/error assignments are bounded engineering data,
not user-facing choices. They are defined once in a shared codec table and
locked by golden vectors used by `net`, `networkd`, and host tests rather than
maintained as two independent parsers.

## Consumed AF_UNIX peer credential contract

p003 provides `SO_PEERCRED` with the fixed-layout public
`struct zedbsd_peercred` containing connector `pid`, `euid`, and `egid`. The
dependency semantics consumed here are:

- credentials are snapshotted when the stream connection is established;
- the accepted endpoint reports the connector snapshot even if that process
  subsequently changes ids or exits;
- the connecting endpoint reports the listening process snapshot defined by
  the final API contract;
- `socketpair` initializes both peer snapshots deterministically;
- the result is available only on a connected AF_UNIX socket and has ordinary
  `getsockopt` length negotiation and errors;
- the connection stores scalar ids, not an indefinitely retained mutable
  credential object;
- accept, failed connect, listener close, peer close, and socket teardown do
  not leak a credential or socket reference.

`networkd` obtains this record immediately after `accept4` and before reading a
request. It associates authorization, cancellation, and audit state with that
snapshot for the lifetime of the request. Root may use every checked operation.
An admitted nonroot peer may use the bounded global WLAN opcodes but not raw
STATIC, DEFAULTROUTE, DNS, generic DHCP, or unrelated mutation opcodes. A ZNV2
request contains no claimed uid, username, home, group membership, credential
path, interface, or passphrase.

An accepted `enable` copies the authenticated peer's effective UID into the
single active policy record; `net` retains no policy state. A later authorized
`enable` atomically switches that record to its authenticated peer, and UID 0
is always allowed to override. Networkd selects `/etc/wifi.conf` for policy UID
0 or `.wifi.conf` below the passwd-record home for another policy UID, without
trusting `HOME`. It uses p005's no-follow, owner/mode, lock, and bounded parser
rules. No client can select a UID or path.

Networkd reads a passphrase only while preparing one `/sbin/wifi connect`
child, passes it on dedicated descriptor 4, and explicitly wipes every daemon
and child copy when that operation terminates. The persistent store remains
the only long-lived passphrase owner. Policy state may retain the UID,
nonsecret selected SSID, interface/device identity, and L3 ownership, but not a
passphrase or PMK.

## Socket access policy

The listener policy is fixed: `/run/networkd.sock` is owned by `root:network`
(GID 69) and has mode `0660`. Creation sets and verifies ownership and mode
before readiness is announced. p003 supplies the `network` group and the
intended members. A process outside root or that group cannot connect; a
successfully accepted endpoint is still authenticated with the p003 snapshot
before ZNV2 dispatch. Mode bits are the admission boundary, not a substitute
for peer identity and per-op validation.

World-writable mode, caller-selected uid/path fields, and authorization based
only on a payload assertion are forbidden. Tests cover wrong owner/group/mode,
startup failure while applying ownership, unauthorized connect, admitted
nonroot peer, root peer, and descriptor/lifecycle cases from p003.

## `/sbin/wifi` child contract

This Phase adds the private machine invocation, `wifi --machine ...`, and its
`WIFI1` record contract for `networkd`. The child contract is:

- interface and operation may be argv because they are validated bounded
  identifiers; a passphrase never appears in child argv or the environment;
- inherited descriptor 4 carries the exact passphrase bytes followed by EOF,
  using p006 `--passphrase-fd=4`. fd 3 remains reserved for init readiness.
  The child rejects short input, more than the WPA2-Personal maximum plus the
  overflow probe, trailing data, and an unexpected secret for a read-only
  operation;
- both parent and child explicitly wipe all secret buffers after use;
- machine stdout is the versioned `WIFI1` record stream, bounded to 32768 bytes
  total and 64 scan records. Human `wifi list` output is not parsed by
  `networkd`;
- stderr is a separate diagnostic channel bounded to 512 bytes. Control bytes
  are normalized and secrets are redacted before returning a diagnostic to
  `net`;
- stdout/stderr are drained while the child runs so a full pipe cannot deadlock
  the deadline. No unbounded temporary output file is used;
- the `WIFI1` terminal record distinguishes success, rejected credentials, no
  matching BSS, timeout/cancel, unsupported security, interface-down, device
  removal, and malformed machine output; p004 exit 0/1/2 and signal status are
  checked independently;
- one child invocation contains p010's complete 30-second userspace retry
  sequence; `networkd` never recreates that retry loop around an active child;
- EOF with a partial record, excess records/bytes, output after terminal
  status, or exit zero without a complete success record is failure.

The `wifi <interface> connect <SSID> <passphrase>` form remains the p004 direct
recovery/debug interface, but networkd must use the secret-descriptor path.
The primitive also provides
`wifi <interface> disconnect`; p007 uses it for explicit down, cancellation,
and rollback rather than treating process termination as L2 disconnection.

## Deadline and cancellation contract

- Request header, request payload, each child stage, and response write have
  finite monotonic deadlines. Header read, payload read, and response write are
  each capped at five seconds and by the remaining operation deadline. Scan
  completion is capped at 15 seconds and one `/sbin/wifi connect` child at 30
  seconds; p007
  additionally caps DHCP at 10 seconds and a compound operation at 90 seconds.
- A child operation receives the remaining total stage deadline; repeated
  irrelevant scan records or EINTR cannot extend it.
- On timeout, client disconnect, service shutdown, or cancellation, networkd
  closes the secret writer, requests operation-specific cancellation, sends a
  bounded termination signal, waits one second, then uses
  SIGKILL only if necessary and always reaps the child.
- Scan cancellation invokes the primitive scan-stop operation when its state
  is uncertain. Association cancellation must either be interruptible in the
  WLAN ioctl or have a separate bounded disconnect/cancel primitive before
  this Phase can complete.
- A killed userspace wrapper must not leave a kernel ioctl, USB callback, key,
  or scan state orphaned. Device removal and networkd shutdown use the same
  retirement barrier.
- `networkd` remains single-request in this Phase. The child runner must not
  accidentally inherit the listener, accepted client, unrelated temporary
  files, or readiness descriptor.

## Planned implementation

1. Materialize the bounded codec/error allocation table and add
   codec/credential/child golden vectors before production changes.
2. Consume the completed p003 peer-credential UAPI and regress its snapshot,
   `getsockopt`, and lifecycle behavior; do not duplicate that implementation.
3. Retain the `root:network` mode `0660` listener; authenticate the p003 peer
   snapshot and reject an unauthorized peer before dispatch.
4. Add a shared strict `ZNV2` codec and bounded I/O helpers. Migrate every
   wired caller and add the Wi-Fi callers in the same change, delete the V1
   parser/executor, and retain only bounded initial-magic version failure.
5. Add the bounded child runner with separate secret, machine stdout, and
   diagnostic pipes; implement full timeout, termination, and reaping paths.
6. Extend p004's `/sbin/wifi` with the private machine/secret-fd mode and add
   production-contract child fixtures. Keep human output outside the machine
   parser.
7. Implement the global Wi-Fi ZNV2 operations needed by p007: empty
   enable/disable/list/disconnect/profiles-changed requests and one bounded
   SSID for connect. Reject every public interface/UID/path/profile/secret
   field. Add the fixed policy-UID store reader boundary without yet composing
   the full selection/DHCP state machine.
8. Run focused host/native tests, sanitizer/analyzer variants where supported,
   existing AF_UNIX rights/socket regressions, networkd ZNV2/fd-3 regressions,
   and the supported `make -j16` build. Do not run aggregate `make check`.

## Verification contract

At minimum, automated evidence covers:

- every split point in the ZNV2 header and payload, short reads/writes, EOF,
  extra bytes, overflow, 32-byte header and 4096/32768-byte payload boundaries,
  maximum-minus/maximum/maximum-plus-one, malformed field ordering,
  unknown/reserved data, and client read timeout;
- SSIDs with spaces and boundary lengths; no ZNV2 request contains a
  passphrase. A daemon-read test passphrase appears only in the checked p005
  reader and fd-4 buffers, is never echoed, and is absent from logs, redacted
  traces, child argv, environment captures, diagnostics, and machine records;
- accepted peer credentials, id change after connect, socketpair, failed
  connect, listener/peer close, and authorization allow/deny matrices;
- root and nonroot `enable` policy-UID selection from peer credentials,
  daemon-side fixed-path reads, owner/mode/symlink checks, owner switching,
  empty profiles-changed delivery, and malicious UID/path/interface/secret
  payload fields having no effect;
- child success, nonzero exit, exec failure, malformed/over-32768-byte stdout,
  sixty-four/sixty-five scan records, over-512-byte stderr, blocked writer,
  crash, 15/30-second timeout, cancellation, one-second SIGTERM grace, SIGKILL
  fallback, and wait/reap failure;
- scan-stop/association cancellation and USB removal without retained child,
  descriptor, key, or driver reference;
- migrated ZNV2 UP/DOWN/DHCP/STATIC/SHOW plus global WIFI_ENABLE,
  WIFI_DISABLE, WIFI_LIST, WIFI_CONNECT, WIFI_DISCONNECT, and
  WIFI_PROFILES_CHANGED; explicit V1 version failure and eventual parser
  absence; fd-3 readiness, direct ifconfig, and wired network regressions.

No real radio, SSID, or credential is required by this Phase.

## Queue-entry engineering checks

These are mechanical checks for a bounded implementation Queue, not unresolved
product or human decisions:

- Generate one shared table for request-ID/opcode ranges, opcode/inner-field
  numbers, inner byte order, malformed-header correlation, and stable errors;
  lock it with golden vectors. The header/request/response/list maxima remain
  32/4096/32768 bytes and 64 records.
- Record the exact p003 `struct zedbsd_peercred` dependency and require its
  automatic evidence before the p006 Queue runs; p006 does not reimplement it.
- Assign the unsupported-version error in that table and include a source/test
  inventory proving the simultaneous caller migration deletes every V1/ZNV1
  parser and executor path.
- Map p003's fixed root/all and admitted-nonroot read-only-plus-WLAN matrix to
  the global opcodes. The socket remains `root:network 0660`, GID 69; the
  authenticated `enable` euid, never a payload UID, becomes policy owner.
- Map every p006 `WIFI1` terminal/error into one ZNV2 result. Secret fd 4,
  32768-byte/64-record stdout, 512-byte stderr, and EOF-terminated secret bytes
  are fixed inputs, not selectable alternatives.
- Add boundary vectors for five-second request/response I/O, 15-second scan,
  30-second direct connect, and one-second termination grace. Cancellation
  uses primitive search-stop or disconnect and must join the kernel operation;
  process termination alone is never the cancel contract.
- Consume p003's root-only direct mutation rule and p003/p004's exact
  status/completed-list query policy. No old `wpa` protocol or database
  compatibility package is permitted.

## Completion conditions

- One `root:network` mode-`0660` `/run/networkd.sock` safely serves strict ZNV2
  wired and Wi-Fi requests with p003 peer-credential authorization; no V1
  request can execute.
- Public WLAN requests contain no interface, UID, path, profile, or passphrase.
  Networkd stores the authenticated active policy UID, selects only its fixed
  p005 path, and rejects an unauthorized peer or operation before any read,
  child, or mutation.
- SSIDs survive length framing, and secrets never appear in child argv,
  environment, machine output, returned diagnostics, or retained temporary
  files; bounded store/fd-4 copies are wiped after use and no passphrase remains
  in policy state.
- Every child/output path is bounded, cancellable, reaped, and removal-safe.
- Focused automatic gates and wired/fd-3 regressions pass; no hardware success
  or public multi-stage orchestration is claimed.

## Interruption and resumption

Before Queue selection, attach the generated codec/error table and the p003-
p005 dependency evidence described above. If the kernel cannot provide stable
peer credentials or an in-flight WLAN ioctl cannot be cancelled safely, record
that exact blocker and return it to the owning lower-layer Phase rather than
weakening authentication or relying on SIGKILL. Resume from the failed
automatic gate with a new bounded Queue; no physical request belongs to p006.

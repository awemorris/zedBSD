# WS005 Phase 004: primitive WLAN ioctl command

Last updated: 2026-08-30

WSID: `ws005`

Phase ID: `p004`

Combined ID: `ws005-p004`

Status: planned; not queued; depends on the generic WLAN UAPI

Parent: [WS005 networking and WLAN](../ws.md)

Tests: [WS005 test index](../tests/README.md)

## Objective

Add `/sbin/wifi` as a finite, non-resident wrapper around the versioned WLAN
network-device ioctls.  It exposes direct root recovery and supplies the fixed
child invoked by `networkd`, but owns only scan, status, WPA2-Personal/CCMP
association, and disconnect at L2.

The command must not read a profile database, remain in the background, run
DHCP, configure an IP address, or mutate routes and DNS.  Automatic profile
selection and L3 composition belong to later `net`/`networkd` Phases.

## Baseline and dependencies

- `ws005-p002` freezes the primitive command names, L2 boundary, and removal of
  the historical `/sbin/wpa` backend.
- `ws005-p003` makes direct mutating WLAN ioctls root-only and supplies the
  authenticated `networkd` path used by ordinary users.
- A WS004 WLAN-UAPI Phase must first publish fixed-size, versioned, LP64/ILP32-
  stable scan-control, scan-snapshot, status, connect, and disconnect requests
  plus a fake `net_device` fixture.
- A selected physical driver is not required for this Phase's automatic
  completion.  Modeled success is not physical radio evidence.

The generic `net_device_ops.ioctl` hook exists, but the current INET ioctl
dispatcher does not forward a WLAN request to it.  That dispatch and its
central read-versus-mutate classification are prerequisites owned with the
WS004 UAPI and p003 privilege work, not private workarounds in this command.

## Queue-entry engineering inventory (read-only)

Before Queue proposal, confirm the final WS004 p027 WLAN request constants,
fixed layouts, state values, and fake-device entry points against
`include/uapi/zedbsd/netif.h`, `include/kern/net/net-device.h`, and
`src/kern/net/inet-socket.c`.  Record which requests are query-only or mutating
under p003 and verify that no existing private driver command collides.  This
is a mechanical header/dispatcher inventory; it cannot substitute a private
`wifi` structure for the frozen WS004 UAPI.

Also inventory the `/sbin` package/install conventions in
`userland/base/ifconfig/Makefile`, `userland/base/dhcpc/Makefile`, and
`userland/base/networkd/Makefile`, plus the existing command and network test
registration points.  The Queue proposal names the finite new files and
fixtures found by that pass.  No additional command, output, timeout, or
security choice is awaiting human judgment.

## Public grammar

```text
wifi INTERFACE search start
wifi INTERFACE search stop
wifi INTERFACE list
wifi INTERFACE status
wifi INTERFACE connect SSID PASSPHRASE
wifi INTERFACE disconnect
```

The internal machine forms used by `networkd` add `--machine` before the
interface.  Connect also uses a dedicated secret descriptor:

```text
wifi --machine INTERFACE search start
wifi --machine INTERFACE search stop
wifi --machine INTERFACE list
wifi --machine INTERFACE status
wifi --machine INTERFACE connect SSID --passphrase-fd=4
wifi --machine INTERFACE disconnect
```

Machine mode and the descriptor form are implementation interfaces, not
additional public credential-management commands.  The descriptor must not be
replaced by a path, environment variable, stdin convention, or shell
substitution.

All other operands, abbreviations, reordered words, unknown options, empty
interface names, and extra arguments are rejected with usage status before an
ioctl.  Interface names use the established `IFNAMSIZ` validation.  A CLI SSID
is 1--32 non-NUL octets; quoting is a shell concern, and embedded NUL cannot be
represented by this v1 argv interface.

The initial passphrase form accepts the WPA2-Personal passphrase range of
8--63 printable octets.  WPA3-SAE, WPA1/TKIP, 802.1X/EAP, WPS, open networks,
raw enterprise credentials, and silent downgrade are rejected rather than
misreported as WPA2-Personal success.

## Primitive operation semantics

### Search start and stop

- `search start` issues the versioned start request and returns after the
  driver has admitted asynchronous scanning, not after a fabricated result is
  available.  One 15-second monotonic window measured from the start request
  covers admission through the resulting complete/failed generation; the
  command may return on admission, but the generation retains that original
  deadline.  Starting an already active scan is idempotent success and does
  not restart it.
- `search stop` cancels future scan production and joins or retires the active
  scan callback under the kernel UAPI contract.  Stopping an idle scan is
  idempotent success.
- Neither operation associates, disconnects an established link, chooses a
  saved network, nor performs a sleep-and-poll loop in the utility.

### List

- `list` obtains one snapshot of at most 64 BSS entries identified by a
  generation.  It never starts a scan and never merges two generations after
  a size retry.
- A result contains only bounded public BSS facts needed by v1: escaped SSID,
  BSSID, channel, signal, WPA2-Personal/CCMP capability, and snapshot state.
  Vendor information elements, arbitrary management frames, keys, challenge
  material, and firmware buffers are not exported.
- The kernel API must prevent count/size arithmetic overflow and distinguish
  an empty completed snapshot, an in-progress scan with no current results,
  a generation race, and an unsupported interface.
- The command has a 32-KiB total output/allocation ceiling.  If the generation
  contains more than 64 entries or cannot fit that byte bound, it carries an
  explicit truncation marker or returns `EOVERFLOW`; it never allocates from
  an untrusted count without a ceiling or labels a truncated view complete.

### Status

`status` returns one state from the frozen kernel model: down, idle, searching,
associating, associated, recovering, or failed.  Associated status may include
escaped SSID, BSSID, channel, signal, and WPA2/CCMP identity.  Recovering
reports only the same-BSS attempt/window state; a failed state includes one
bounded stage/error code.  No passphrase, PMK, derived PSK, temporal key,
nonce, or replay counter is returned.

### Connect

- `connect` submits exactly the named SSID and one passphrase to the versioned
  connect ioctl, then waits through the UAPI's bounded terminal-state
  mechanism for at most 30 monotonic seconds.  Success means authenticated and
  associated L2 carrier; it does not mean an IPv4 lease exists.
- A second connect cancels or replaces prior pending association only according
  to the frozen kernel transaction; the command never reports both operations
  as successful.
- Rejection, no matching BSS, authentication failure, association rejection,
  key failure, radio removal, cancellation, and timeout remain distinct
  diagnostics.
- The plaintext and all derived userspace copies are kept in bounded mutable
  storage, excluded from diagnostics, explicitly cleared on every exit path,
  and not retained after the ioctl has copied its request.

The public argv form has the user-accepted process-argument and shell-history
exposure documented by p002.  This Phase does not call that exposure secure or
attempt to hide it with misleading redaction claims.

For the internal descriptor form, only the literal descriptor 4 is accepted;
fd 3 remains reserved for init readiness.  `/sbin/wifi` reads at most the
maximum passphrase plus one overflow byte, requires EOF, accepts the exact
bytes without whitespace trimming, closes fd 4 immediately, and rejects
short, long, unreadable, repeated, or trailing data.  The public process argv
contains only the fixed descriptor number.  The writer closes its copy on
every fork/exec failure so neither side can wait forever for EOF.

### Disconnect

`disconnect` cancels a pending scan or connection, disassociates an established
link, clears transient driver keys, and publishes carrier down before success.
It is idempotent when already disconnected.  It does not clear `IFF_UP`, an IP
address, route, resolver file, or persisted profile.  The later compound
`net wifi down` operation owns that L3/interface composition.

After unexpected link loss, the common WLAN core may use only the retained PMK
to recover the same accepted BSS after 0/1/2/4/8-second delays, with at most
five failed attempts within one 30-second window.  `status` exposes a
recovering state and keeps secure carrier down until reauthentication
succeeds.  Exhaustion publishes failed/carrier-down state.  `/sbin/wifi`
remains one-shot and never implements that loop or selects another profile;
the user or desktop starts a new policy attempt with `net wifi up`.  Explicit
disconnect cancels recovery and clears the PMK and transient keys.

## Stable output and exit contract

Human mode produces administrator-facing output.  `networkd` invokes
`--machine` and never parses that prose.  Machine mode emits a bounded,
version-tagged `WIFI1` record format with fixed record types, field names,
escaping, and exactly one terminal result.  Backslash, quote, tab, newline,
carriage return, and non-printable SSID octets are escaped unambiguously; one
BSS occupies one complete record.  Field order and numeric base are stable.
Machine errors carry a typed stage and errno/status but no human-localized
text or secret.

Human explanation and errors go to the normal human stdout/stderr channels.
They may name the interface, operation, public state, and bounded error, but
never the passphrase or key.  Machine mode keeps diagnostic prose on stderr
and its structured result on stdout; `networkd` consumes only the structured
bounded record and the exact exit/signal status.

Exit values are:

- `0`: the requested primitive reached its defined success boundary;
- `1`: a runtime, ioctl, cancellation, timeout, or device failure; and
- `2`: invalid CLI syntax, bounds, or unsupported public option.

A signal exit remains a signal to `networkd`; it is not normalized to success.
One invocation emits at most 32 KiB of primary structured/human stdout,
including at most 64 BSS records and one terminal record.  The separate
redacted diagnostic stream is at most 512 bytes including its terminator.
Scan admission/completion is capped at 15 monotonic seconds and direct connect
at 30 monotonic seconds.  These are fixed v1 limits; reaching one returns a
typed overflow or timeout rather than partial success.

## Security and ownership rules

- `status` and completed `list` are classified query-only by the kernel.  Start,
  stop, connect, secret/key installation, and disconnect are mutating and need
  effective UID 0 under p003.
- The CLI does not attempt its own weaker authorization as a substitute for
  the kernel.  It may preflight `geteuid()` only to improve the error message.
- No environment variable supplies an interface, SSID, passphrase, descriptor,
  timeout, or output mode.
- All sockets and inherited descriptors are `CLOEXEC` except secret fd 4
  during the intended child exec; it is closed immediately in the new image.
- Core dumps, syslog, test failure output, analyzer traces, and retained QEMU
  logs use dummy credentials and redaction checks.
- `/sbin/wifi` is installed mode `0755`, not setuid or setgid.  Authority comes
  from the caller or root `networkd`, never the file mode.

## Non-goals

- credential database parsing or `net wifi set-key`;
- `networkd` client framing, peer authorization implementation, auto-selection,
  DHCP, address/route/DNS cleanup, or boot policy;
- a resident scan, reconnect, roaming, or lease-renewal daemon;
- firmware loading or a selected USB WLAN driver;
- WPA3, enterprise authentication, AP/monitor mode, regulatory-domain UI, or
  arbitrary 802.11 frame exposure; and
- claiming that the fake UAPI fixture emulates radio hardware.

## Ordered implementation packages

1. Import only the frozen WS004 public WLAN header and add compile-time request
   layout assertions; do not duplicate ioctl structures in userland.
2. Implement strict command/interface/SSID/passphrase/descriptor parsing and
   fixed exit/diagnostic helpers.
3. Add search start/stop, generation-safe list, and status ioctl mappings with
   separate human rendering and canonical machine `WIFI1` records.
4. Add public-argv and internal-secret-FD connect paths sharing one zeroizing
   request helper.
5. Add idempotent disconnect and exact kernel error/stage propagation.
6. Install the non-setuid `/sbin/wifi` binary and production-source fake ioctl
   hooks without changing ordinary image startup behavior.
7. Run the focused, sanitizer/analyzer, build, regression, and bounded QEMU
   gates below.

## Verification plan

Use a fake WLAN `net_device` connected to the production ioctl dispatcher and
the production command parser.  Cover every valid command plus empty/long/bad
interface names, arbitrary SSID escaping, passphrases at 7/8/63/64 octets,
unknown security, extra operands, rejected descriptor spellings 0/3/5/nondecimal,
short/long/blocked/failed secret-fd input, and closure on every error.

State fixtures cover repeated start/stop/disconnect, empty/in-progress/complete
scan generations, generation replacement during list, count/byte overflow,
each status, connect success and every terminal stage, timeout, signal, device
detach, concurrent disconnect, same-BSS recovery at attempts 0/1/5/6 and
29.999/30.000 seconds, recovery cancellation, and no stale carrier/key state.
Primary output covers 32767/32768/32769 bytes and 63/64/65 BSS records;
diagnostics cover 511/512/513 bytes without splitting an escape or field.

Security fixtures inspect the child's argv and captured human/machine
stdout/stderr to prove that the internal path contains only the FD number and
that no secret appears on success, parse failure, ioctl failure, timeout,
signal, or sanitizer report.
Hooks must prove that no command invokes `dhcpc`, writes a credential/config
file, changes IPv4/routes/DNS, or remains resident.

Run ordinary and ASan/UBSan variants where supported, the compiler analyzer,
affected network-device/ioctl/AF_UNIX/networkd regressions, `make -j16`, a
bounded `qemu-system-x86_64` fake-WLAN command cell, and `git diff --check`.
Do not use aggregate `make check` or `.internal/` material.

## Acceptance conditions

- All six public primitive commands map to the exact WLAN ioctl operation and
  L2 boundary above; invalid input reaches no ioctl.
- List and status are generation-consistent and bounded; human and machine
  modes are separate, machine records are canonically escaped, and neither
  contains a secret or arbitrary firmware/frame payload.
- Scan output never exceeds 64 entries or 32 KiB, diagnostics never exceed
  512 bytes, and the 15-second scan and 30-second connect deadlines do not
  restart on retry or an idempotent request.
- Both connect input paths reach the same association result; the internal
  child argv has no passphrase and names only fd 4, all secret
  buffers/descriptors retire, and every failure retains its exact stage.
- Disconnect joins pending work, clears transient keys, and publishes carrier
  down without touching persisted credentials or L3 state.
- Status exposes bounded same-BSS recovery without a passphrase or PMK; five
  failed attempts or 30 seconds is terminal, explicit disconnect cancels it,
  and no primitive process or cross-profile policy loop remains resident.
- Non-root direct mutation is rejected by the kernel, root recovery works, and
  the installed binary has no setid bit.
- Existing wired `ifconfig`, `net`, `networkd`, `dhcpc`, AF_UNIX, build, and
  QEMU boot regressions remain passing.
- Evidence is explicitly labeled fake/model; selected-adapter scan and
  association remain later physical gates.

## Reconsideration boundary

Return to planning if the WLAN UAPI cannot return a coherent bounded snapshot,
if connect requires a long-lived userspace authentication engine, or if a
secret must be exposed in the internal argv or human output.  Extract the
missing kernel/UAPI foundation or revisit p002 explicitly.  Do not silently
resurrect `/sbin/wpa`, add a daemon loop to `wifi`, parse firmware-private
buffers in userland, or broaden direct ioctl privilege.

## Queue boundary and handoff

This Phase is not selected for execution.  Its v1 bounds are frozen, but it may
enter a future Queue only after the generic WLAN UAPI, p003 privilege boundary,
a finite timebox, and explicit approval.  Credential persistence is separately
owned by `ws005-p005`; `networkd` composition and physical radio acceptance
remain later Phases.

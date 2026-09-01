# WS005 Phase 007: `net wifi` orchestration

Last updated: 2026-09-01

Phase ID: `ws005-p007`

Status: planned; not queued; follows p009 and p006

Parent: [WS005](../ws.md)

Protocol dependency: [networkd Wi-Fi control protocol](../phase006-networkd-wifi-protocol/phase.md)

Tests: [WS005 test index](../tests/README.md)

Architecture: [WLAN v1 control contract](../phase002-wlan-v1-contract/phase.md)

Authentication: [AF_UNIX peer credentials](../phase003-unix-peer-credentials/phase.md)

Primitive: [wifi ioctl command](../phase004-wifi-ioctl-command/phase.md)

Profiles: [Wi-Fi credential store](../phase005-wifi-credential-store/phase.md)

## Objective

Add the public Wi-Fi control grammar to `/sbin/net` and make `networkd` own
the complete L2-to-DHCP transaction:

```text
user or desktop -> /sbin/net -> /run/networkd.sock -> networkd
                                                    +-- /sbin/ifconfig
                                                    +-- /sbin/wifi
                                                    `-- /sbin/dhcpc
```

`/sbin/wifi` remains a primitive WLAN ioctl wrapper and stops at L2. `net`
selects and reads its own euid's profile, sends only the bounded values needed
for the operation in ZNV2, and never asks networkd to open a profile.
`networkd` authenticates the peer with p003, invokes only absolute child paths,
applies one total deadline, and either commits a coherent connected/DHCP state
or executes the fixed fail-clean transition. During `up`/`connect`, no
passphrase is placed
in `net` argv, networkd child argv, the environment, machine output, or
diagnostics; it exists only in bounded wiped profile/ZNV2/secret-fd buffers.

## Dependencies

- The lower WS004 WLAN UAPI/driver Phases and WS005
  [p004](../phase004-wifi-ioctl-command/phase.md) primitive provide bounded
  scan, list, status, connect, disconnect/cancel, link state, and removal.
- WS005 [p005](../phase005-wifi-credential-store/phase.md) provides the strict
  net-side config parser/writer, euid path selection, profile order, locking,
  atomicity, and secret lifetime.
- `ws005-p006` provides one credential-aware ZNV2 socket, strict SSID framing,
  the private wifi machine protocol, the secret descriptor, bounded output,
  and child cancellation/reaping.
- The existing WS002 `ifconfig`, `dhcpc`, `net`, and `networkd` behavior is the
  wired compatibility baseline.
- The kernel must expose `IFF_RUNNING` only after a secure association is
  usable for Ethernet-II traffic. `dhcpc` may not be started merely because a
  scan found a BSS or an authentication attempt began.

## Scope

- the exact public `net wifi` grammar and script-visible result rules;
- euid-based `net` selection of `/etc/wifi.conf` or the invoking user's
  `~/.wifi.conf`, with no profile access in networkd;
- atomic key persistence, auto-connect metadata, and secret redaction;
- networkd composition of `/sbin/ifconfig`, `/sbin/wifi`, and `/sbin/dhcpc`;
- idempotent search start/stop, bounded list output, explicit connection, and
  automatic candidate selection;
- one per-request transaction with coherent success, fail-clean, and degraded
  outcomes;
- failure injection at every child/protocol/state transition;
- retained wired commands, direct-ifconfig recovery, and fd-3 regressions.

## Non-goals

- implementing DHCP inside `/sbin/wifi` or the WLAN driver;
- adding a persistent `wpa` child, a second daemon socket, or `/etc/wpa/`;
- duplicating Wi-Fi credentials in `/etc/net.conf`;
- placing a passphrase on a networkd child command line, or on the public
  `up`/`connect` command line; the specified `set-key` grammar necessarily has
  a passphrase operand and receives separate exposure documentation;
- adding a boot/login resident multi-profile selection policy loop;
- defining multi-interface routing, DNS ownership, or confirmed-commit policy
  beyond the fail-clean ownership needed for this command family;
- claiming physical Archer success, which belongs to `ws005-p008`.

## Public command grammar

The required initial grammar is:

```text
net wifi search start <interface>
net wifi search stop <interface>
net wifi list <interface>
net wifi set-key <SSID> <passphrase> [auto]
net wifi up <interface>
net wifi down <interface>
net wifi connect <interface> <SSID>
```

`list` is part of this Phase so callers do not bypass networkd and parse
human-oriented `/sbin/wifi` output. Networkd consumes the bounded p006
machine records and emits one stable, escaped public view containing SSID,
BSSID, RSSI, channel/band, WPA2-Personal/CCMP capability, snapshot state, and
generation. The existing interactive `net` console exposes the same operations
through shared handlers and adds no device-management mode. WLAN v1 adds no
second public machine option: it renders one canonical escaped record per BSS
from the already-versioned internal ZNV2/`WIFI1` records.

Grammar and parsing rules are:

- every command has the exact arity above; unknown options and extra operands
  fail before sending a request;
- `<interface>` is a bounded interface identifier and is never treated as a
  path;
- SSID is one argv operand for the public CLI. A shell user quotes whitespace
  in the ordinary way; after argv parsing, ZNV2 preserves its explicit byte
  length;
- `auto` is the only accepted `set-key` flag and is literal, not a truthy
  prefix;
- the public `set-key` form necessarily receives the passphrase in the
  invoking `net` process's argv. `net` copies it into a bounded wiped buffer,
  atomically updates its euid-selected profile locally, and does not send this
  storage-only operation to networkd;
- for `up`, `net` sends a bounded set of auto-enabled profile values in ZNV2;
  for `connect`, it sends only the exact matching profile value. Networkd
  validates bounds, never treats a request value as a path, wipes the payload
  secret after use, and forwards it to `/sbin/wifi` only on the p006 secret fd;
- success and failure diagnostics name the operation and stage but never print
  the passphrase, PMK, unescaped SSID bytes, complete request, or credential
  record.

## Primitive `/sbin/wifi` boundary

`networkd` calls the production primitive using the private p006 machine mode.
The corresponding human recovery/debug surface remains:

```text
wifi <interface> search start
wifi <interface> search stop
wifi <interface> list
wifi <interface> status
wifi <interface> connect <SSID> <passphrase>
wifi <interface> disconnect
```

The direct `connect` spelling is not used by networkd because it exposes the
passphrase in argv. Its private invocation receives the secret through the
dedicated descriptor. Search and list are L2 operations; neither primitive
command starts DHCP, edits a route, writes resolver state, or persists a key.

The machine status must distinguish at least interface missing/down, scan in
progress/complete/not started, no matching BSS, unsupported security,
authentication rejected, association timeout, cancelled, device removed, and
malformed/oversized output. A generic exit status without a terminal record is
not sufficient for transaction decisions.

## Credential database contract

`net` selects the database from its own effective uid, not from `HOME`, an
argument, or a ZNV2 field:

- effective uid 0 selects `/etc/wifi.conf`;
- a nonroot process selects `.wifi.conf` beneath the account home resolved for
  that euid through the account database;
- a request cannot name a uid, username, home, alternate file, or system/user
  mode;
- networkd never opens, stats, resolves, parses, or writes either profile;
- one database entry stores p005's explicit-length SSID, WPA2-Personal/CCMP
  passphrase, and `auto` or `manual` policy. It never stores interface,
  address, route, DNS, or DHCP data;
- local `net wifi set-key` replaces the matching SSID entry in place or
  appends it atomically while preserving every unrelated valid entry and its
  order; omission of `auto` writes `manual` and clears a prior auto flag;
- a write uses a same-directory temporary file, mode `0600`, ownership for the
  selected identity, validation, sync, and atomic rename. Failure leaves the
  prior valid file authoritative and removes no usable entry;
- symlinks, wrong owner, group/world-accessible secrets, duplicate entries,
  malformed lengths/escapes, excessive records, or an oversized file fail
  closed;
- parser, writer, diagnostic, crash, and allocation paths wipe temporary
  passphrase/PMK buffers and never copy secrets to logs or response text.

For `up`, `net` sends only bounded auto-enabled entries needed for selection;
for `connect`, it sends only the requested matching entry. The ZNV2 request has
no profile path or uid assertion. Networkd uses the p003 peer snapshot to admit
and audit the operation but does not reselect or reopen the caller's file.
The p005 auto-profile bound and ZNV2 encoding must fit the 4096-byte request;
if the selected set cannot fit, `net` fails before network mutation rather than
silently dropping or reordering an entry.

The `wifi-conf 1` encoding, duplicate policy, plaintext WPA2 passphrase model,
owner/mode rules, account-home open procedure, 32-KiB/64-profile/4096-secret-
byte limits, five-second lock wait, and shared-reader consistency are fixed by
p005. `/etc/wifi.conf` and `~/.wifi.conf` are owned by WS005 and are not part
of `/etc/net.conf`.

## Operation contracts

### `search start`

1. Authorize the peer for read/mutate control of the named interface.
2. Verify that the interface is a WLAN device. If it is down, bring it
   administratively up and leave it up. If it is already securely associated,
   return `busy-associated` without disrupting that link; v1 does not perform
   background roaming scans.
3. Invoke the bounded primitive scan-start operation.
4. Return success for an already-running scan only if the driver confirms the
   same scan generation is healthy; do not spawn duplicate scans.

Starting a v1 scan does not connect, read a credential, start DHCP, or stop a
current secure association. An already associated interface returns the fixed
`busy-associated` result above; background roaming scan is outside v1.

### `search stop`

Cancel the active scan generation, wait for its bounded terminal state, and
return idempotent success if no scan exists. Cancellation must retire driver
and USB work before success; killing only the wifi wrapper is not success.
Stopping a scan does not disconnect an established association or erase the
last complete snapshot.

### `list`

If a completed or incrementally readable scan generation exists, return its
bounded results. Otherwise return a stable `not-scanned`/`in-progress` status;
do not silently start a scan. Each record carries explicit SSID length and the
minimum selection metadata needed by p007, with SSID bytes escaped for the
public view. Keys and credential presence are not exposed. Record ordering,
duplicate records, and stale generations follow the p006 `WIFI1` snapshot
exactly; scanning, complete-with-zero-results, and failed are distinct, and
the minimum public fields are fixed above.

### `set-key`

Select the local database from `net`'s euid, validate SSID/passphrase and the
optional literal `auto`, atomically replace one logical SSID record, wipe the
argv/profile secret, and return only a nonsecret result. This operation does
not contact networkd, scan, connect, bring an interface up, or run DHCP.

### `up`

`net wifi up <interface>` is one bounded transaction:

1. In `net`, select and securely load the euid profile, retain only a bounded
   set of auto entries, and send those selected values in ZNV2. In networkd,
   authenticate the p003 peer and snapshot interface/search/association and
   owned L3 state before mutation.
2. Bring the interface administratively up with `/sbin/ifconfig` if required.
3. If no usable scan is active, start one. Wait at most 15 seconds for bounded
   completion or a bounded set of usable results from the same generation.
4. Match only entries marked `auto`. Preserve p005 file order, with no hidden
   RSSI priority between profiles. For multiple compatible BSSIDs of one SSID,
   select strongest RSSI and then the numerically lowest BSSID as the tie
   break. Make at most four actual connection attempts.
5. For each permitted candidate, invoke private `/sbin/wifi` connect with the
   key on the secret descriptor. Wait for the secure association and
   `IFF_RUNNING`, and stop trying on cancellation, device removal, or total
   deadline exhaustion.
6. After a candidate connects, stop the search and wait for bounded scan
   retirement.
7. Invoke `/sbin/dhcpc <interface>` with a 10-second cap and only the remaining
   transaction deadline. Validate its terminal result and resulting
   interface/default-route/resolver ownership.
8. Commit the association and lease state, wipe all loaded secrets, and return
   the selected SSID and BSSID in the canonical escaped public form.

The complete compound operation has one 90-second monotonic deadline. With no
auto entry or no visible matching candidate, leave the interface up and search
active and return the stable `no-configured-candidate` result. Four
authentication failures or DHCP failure enter the fixed p002/p007
fail-clean policy rather than becoming an implicit partial success.

### `connect`

`net wifi connect <interface> <SSID>` requires an already administratively-up
WLAN interface. `net` selects its euid database, rejects an unknown SSID
without prompting, and sends only that selected entry in ZNV2. Networkd starts
or reuses search according to the frozen rule, connects exactly that logical
SSID with a private secret descriptor, and applies the same
association/fail-clean safety as `up`.

`connect` retires stale managed L3 state from a previous SSID, applies the
30-second direct association cap, and reacquires DHCP with a 10-second cap
inside the same 90-second total deadline. It never leaves the previous SSID's
lease, route, or resolver ownership attached to a newly associated network.

### `down`

Cancel an in-flight p007 transaction for the interface, stop search, retire
association work, invoke primitive disconnect, remove the interface's managed
DHCP address, owned routes, and resolver state, and bring it administratively
down. The command is idempotent only after complete retirement; partial L3
cleanup is a reported failure, not down success.

### Post-success link loss

The WS004 p030 common/driver layer may reconnect only the already accepted
network, retaining the PMK but scrubbing PTK/GTK/nonces and retrying after
0/1/2/4/8 seconds, with at most five failed attempts or 30 seconds in one
generation. Carrier is down while the controlled port is closed.

WLAN v1 has no resident networkd policy loop which rereads wifi.conf, switches
to another `auto` profile, or retries forever. If the bounded same-network
reconnect exhausts, the user or desktop invokes `net wifi up <interface>`
again. That invocation treats any retained managed lease, route, and resolver
state as stale input to remove or replace before the new DHCP result commits.

## Transaction and fail-clean model

`networkd` is globally single-flight in v1, so each p007 request runs alone and
the next accepted request is not dispatched until the active transaction and
all cleanup are terminal. Introducing concurrent per-interface dispatch would
require a later Phase and is not inferred from this state machine.

Before the first mutation, validate authorization, request/profile bounds,
interface type, child contracts, and all input which can fail without changing
state. Snapshot the ownership facts needed for cleanup:

- administrative flags and secure carrier state;
- active scan generation and whether p007 started it;
- associated SSID/BSSID/security identity without logging a secret;
- interface addresses, netmask, broadcast address, and DHCP ownership;
- default and interface routes owned by the transaction;
- resolver state and its owning source if `dhcpc` can change it.

Every stage consumes one monotonic total deadline. Retries never reset it. A
validation failure before mutation leaves prior state untouched. The special
`no-configured-candidate` result intentionally leaves the interface up and
search active as specified above; it is not a rollback failure.

After the first mutation, any child timeout, client disconnect, malformed
machine record, cancellation, device removal, authentication exhaustion, or
DHCP failure executes one fail-clean sequence:

1. Cancel and join DHCP, scan, connect, and p030 reconnect work started or
   encountered by the transaction.
2. Invoke primitive `wifi disconnect`, close the controlled port, clear
   transient keys, and prove carrier down.
3. Remove only WLAN/DHCP addresses, owned routes/default route, and resolver
   contributions managed by this interface/transaction. Resolver
   reconstruction preserves every other active interface.
4. Leave the WLAN interface administratively up, disconnected, and
   search-stopped, with no managed WLAN L3 state.
5. Wipe all profile/request/child secret copies and return the initiating stage
   plus fail-clean result.

Fail-clean never reconnects the prior SSID and never restores its old lease,
route, or resolver data. This rule is identical whether the transaction began
down, up/unassociated, or associated. If any child/kernel/USB work cannot be
proven retired, carrier remains uncertain, or owned L3 cleanup cannot be
proven complete, return `degraded` with the initiating and cleanup stages and
the known/unknown state; do not report success or fabricate the fail-clean
target. Cleanup which merely runs `ifconfig down` is forbidden.

## Planned implementation

1. Materialize the fixed command/output/state/error tables and add
   command/state-machine golden vectors before production changes.
2. Add strict `net wifi` argv parsing and ZNV2 construction for bounded values
   selected by `up` and `connect` through the shared p005 parser.
3. Consume p005's euid-selected wifi.conf parser/writer and local `set-key`
   evidence; do not duplicate profile selection or storage in networkd.
4. Add non-composite search start/stop/list handlers through the p006 machine
   child runner and integrate the already-local p005 `set-key` grammar.
5. Add an explicit networkd transaction object with total deadline, snapshot,
   stage, cancellation, owned-resource, and fail-clean state.
6. Add `up`, `connect`, and `down` composition using only absolute
   `/sbin/ifconfig`, `/sbin/wifi`, and `/sbin/dhcpc` child paths.
7. Inject every stage failure and prove unchanged-before-mutation,
   fail-clean-after-mutation, and degraded-retirement results before any
   physical test is requested.
8. Run focused host/native tests, sanitizer/analyzer variants where supported,
   current networkd/fd-3/wired regressions, and the supported `make -j16`
   build. Do not run aggregate `make check`.

## Verification contract

Automatic evidence includes:

- exact-arity parsing, whitespace/boundary SSIDs, unknown flag, invalid
  interface, and safely escaped output for every public command;
- root/system and nonroot/account-home database selection based only on
  `net`'s euid, including malicious `HOME`, path operand attempts, symlink,
  owner, mode, malformed, duplicate, maximum, atomic-write interruption, and
  recovery cases; networkd is proven not to open a profile;
- absence of every test secret from argv captures (except the unavoidable
  public `net set-key` process itself), environment, child argv/diagnostics,
  machine output, non-profile temporary files, and returned text; ZNV2 capture
  tests instead prove exact bounds, accepted-op scoping, wiping, and no echo;
- controlled `ifconfig`, `wifi`, and `dhcpc` child doubles implementing the
  production command contracts, which verify exact absolute argv, descriptor
  inheritance, ordering, remaining deadline, output bounds, cancellation,
  exit, and reaping without a real radio;
- search already-started/not-started/in-progress/complete/stale/cancelled and
  list empty/duplicate/64/65-record/malformed cases, including the 15-second
  cap;
- no auto entry, no matching BSS, file-order candidate rejection then success,
  four-attempt cap, all rejected, secure-carrier timeout, scan-stop failure,
  DHCP timeout/failure,
  client disconnect, child crash, malformed output, and device removal;
- unchanged state for every pre-mutation failure and the same
  admin-up/disconnected/search-stopped/no-owned-L3 fail-clean result after
  mutation from down, up/unassociated, and previously associated states; the
  prior SSID is never reconnected and stale L3 is never reinstalled;
- 30-second association, 10-second DHCP, 90-second total, and one-second child
  termination boundaries, including the four-attempt budget and proof that a
  retry never resets the total deadline;
- carrier loss may launch only p030's same-network 0/1/2/4/8-second, five-
  failure/30-second reconnect generation with PMK-only retention; it never
  switches profiles, and exhaustion requires explicit `net wifi up` which
  replaces stale managed L3 state before DHCP commit;
- no auto entry and no visible match both leave the interface up and the scan
  active with `no-configured-candidate`, without starting DHCP or trying an
  unconfigured SSID;
- migrated wired ZNV2 behavior and explicit legacy-version failure, direct
  ifconfig, `dhcpc`, fd-3 readiness, and service restart regressions.

No physical adapter or access point is required by p007.

## Queue-entry engineering checks

These checks materialize the fixed contract for a bounded implementation
Queue; none requires another product or human policy choice:

- Generate the canonical `net wifi list` renderer and golden records with
  SSID/BSSID/RSSI/channel/band/WPA2-CCMP/state/generation. V1 has no second
  public machine option.
- Import p006's generated ZNV2 profile-value fields for `up`/`connect`, and add
  secret-lifetime, wipe, and redacted-error vectors. Request size is 4096 bytes
  and actual connection attempts are capped at four.
- Import p005's wifi-conf v1 grammar/path/ownership and final record/file,
  lock-wait, and reader-consistency constants. The selected auto set must fit
  one 4096-byte request without truncation or reordering.
- Map p003's admitted-nonroot WLAN policy to the p006 handlers. Profile writes
  remain local and obey filesystem ownership/mode.
- Lock search-start tests to its fixed behavior: bring a down WLAN interface
  administratively up and leave it up; return `busy-associated` without
  starting a disruptive background scan on an associated v1 interface.
- Lock auto selection to p005 profile order, strongest compatible BSSID then
  lowest-BSSID tie break, four actual attempts, canonical selected SSID/BSSID,
  and `no-configured-candidate` leaving interface up/search active.
- Materialize ownership markers and the single fail-clean state machine.
  Global single-request networkd serialization remains the concurrency model;
  timeout/client disconnect cancels and joins the active transaction before
  the next request is dispatched.
- Map p030's same-network 0/1/2/4/8-second, five-failure/30-second, PMK-only
  reconnect into status and cleanup. It never becomes a resident networkd
  multi-profile loop; exhaustion requires another explicit `net wifi up`.

## Completion conditions

- The required public grammar, including `list`, is strict and documented.
- Root and nonroot `net` processes read and atomically update only their
  euid-selected wifi.conf database, with no networkd profile access, arbitrary
  path selection, or secret echo.
- `search`, `list`, `set-key`, `up`, `down`, and `connect` use only their
  intended local/profile or primitive-child boundaries, remain bounded and
  cancellable, and report stable stages.
- `up`/`connect` success means a secure L2 association plus committed DHCP.
  After mutation, failure reaches admin-up/disconnected/search-stopped with no
  owned WLAN L3 state, or explicitly reports degraded retirement; it never
  reconnects the prior SSID or installs stale L3 data on another SSID.
- All automatic fixtures and wired regressions pass before p008 requests one
  physical action; p007 itself makes no hardware-success claim.

## Interruption and resumption

Before Queue selection, attach the generated command/output tables and the
p003-p006 dependency evidence above. A protocol, credential, child,
driver-cancellation, or carrier blocker returns to its owning lower Phase
rather than being patched inside orchestration. Preserve the first failed
stage, ownership snapshot, child status, fail-clean/degraded status, and
bounded redacted diagnostics. Resume from a newly approved automatic Queue;
do not ask for an Archer test from p007.

# WS005 Phase 005: root and per-user Wi-Fi credential store

Last updated: 2026-08-30

WSID: `ws005`

Phase ID: `p005`

Combined ID: `ws005-p005`

Status: Uncleared (`q041`, 2026-08-31); host implementation complete, native
VFS prerequisites assigned to `ws001-p015` and `ws001-p016`

Parent: [WS005 networking and WLAN](../ws.md)

Tests: [WS005 test index](../tests/README.md)

## Objective

Add one strict, versioned plaintext Wi-Fi profile format and the local
management command:

```text
net wifi set-key SSID PASSPHRASE [auto]
```

Effective UID zero reads and writes `/etc/wifi.conf`; another effective UID
reads and writes `.wifi.conf` in that account's passwd-record home directory.
The update is owner-checked, mode `0600`, locked, validated, same-directory
atomic, and redacted on every failure.

This Phase manages profiles only.  `set-key` does not contact
`networkd`, invoke `/sbin/wifi`, scan, associate, or run DHCP.  Later `net wifi
up` and `net wifi connect` use the same parser in the `net` process, select a
profile under the caller's effective UID, and send bounded selected values over
the authenticated `networkd` socket.  `networkd` never resolves a client home
directory or opens a client credential file.

## Baseline and supersession

The earlier WS005 proposal reserved a root-only `/etc/wpa/` database consumed
by a pluggable `/sbin/wpa` child.  That database, child, and backend contract
were never implemented and are superseded by the p002 topology.  No migration
or compatibility parser is required.

`/etc/net.conf` remains WS011's non-secret network-intent file.  A passphrase,
PSK, or credential-file pathname must never be copied into `net.conf`, an
rc.conf service option, or a confirmed-commit rollback program.

## Dependencies

- `ws005-p002`: caller-owned profile selection, WPA2-Personal/CCMP v1, fixed
  command grammar, and no `/sbin/wpa` compatibility path.
- Existing passwd/group lookup, `openat`, `renameat`, `O_DIRECTORY`,
  `O_NOFOLLOW`, `O_EXCL`, record locking, `fsync`, and effective-UID support.
- [`ws001-p015`](../../ws001-posix/phase015-credential-aware-vfs-creation/phase.md)
  must make newly created native objects inherit the caller's effective
  UID/GID.  The current VFS authorizes with the credential but drops it before
  backend creation, so a non-root caller cannot create an honestly owned
  lock or temporary file.
- [`ws001-p016`](../../ws001-posix/phase016-directory-fsync/phase.md) must make
  directory `fsync()` durable or explicitly unsupported.  The current generic
  fallback can return success without synchronizing an overlay directory or
  its backing namespace.
- `ws005-p003` is not required for local `set-key`, but later profile use over
  `networkd` depends on its peer-authenticated socket.

## Queue-entry engineering inventory (read-only)

Before Queue proposal, record the existing `net` parser/model split in
`userland/base/net/main.c` and `userland/base/net/netconf.c`, effective-ID and
passwd lookup behavior in `userland/base/libc/account.c`, and the available
`openat`/`renameat`/`fsync`/record-lock interfaces in the public libc headers
and syscall implementation.  Review the persistent lock and atomic writer
patterns in `userland/base/service/rcconf.c` only as implementation precedent;
they do not alter the stricter credential rules here.

The inventory also identifies the finite source, package, and focused-fixture
registration changes needed in `userland/base/net/Makefile` and the relevant
test Makefiles.  It is read-only Queue preparation, not a format/path/locking
decision and not authority to create either credential file on a live system.

## Path and caller selection

The `net` process selects exactly one store before opening any profile data:

| Calling effective UID | Store |
| --- | --- |
| `0` | `/etc/wifi.conf` |
| nonzero | `<pw_dir>/.wifi.conf`, where `pw_dir` comes from `getpwuid_r(geteuid())` |

The rules are fixed:

- real UID does not override effective UID; `sudo net ...` intentionally uses
  `/etc/wifi.conf`;
- `$HOME`, shell expansion, a command-line path, a request-supplied UID, and
  the `networkd` environment are never used to choose the file;
- a missing, duplicate, malformed, or home-less passwd record is a bounded
  error and cannot fall back to `/etc`, `/root`, the current directory, or an
  environment home;
- root does not merge a user's file and a user does not fall back to the root
  file; and
- the selected directory is opened once as a directory descriptor and all
  target, lock, temporary, validation, and rename operations are relative to
  that stable descriptor.

`networkd` receives selected profile values in a later Phase.  It rejects a
credential path or claimed home in protocol data and does not repeat this
selection algorithm.

## Version 1 file grammar

The initial canonical text format is:

```text
wifi-conf 1
network "escaped SSID" wpa2-personal-ccmp "escaped passphrase" auto
network "another SSID" wpa2-personal-ccmp "another passphrase" manual
```

The normative grammar is:

```text
file        = header newline *record
header      = "wifi-conf 1"
record      = "network" SP quoted-ssid SP
              "wpa2-personal-ccmp" SP quoted-passphrase SP
              ("auto" / "manual") newline
```

There are no comments, continuations, unknown record types, alternate token
orders, duplicate fields, blank lines, unversioned input, environment
substitutions, or shell escapes.  A final newline is required.  Tabs outside
quoted data, trailing tokens, CRLF, embedded NUL, raw control/non-ASCII syntax,
and input beyond any frozen byte/count bound are rejected.

Quoted data uses `\"`, `\\`, `\t`, `\n`, `\r`, and `\xHH` with exactly two
hex digits.  The parser decodes to bytes before applying semantic bounds and
the writer uses one canonical shortest safe escape for every byte.  Although
v1 passphrases are printable and do not need control escapes in valid input,
one shared decoder avoids a second injection-prone grammar.  A decoded SSID is
1--32 non-NUL octets.  A decoded WPA2-Personal passphrase is 8--63 printable
ASCII octets.  SSIDs remain opaque byte strings rather than UTF-8 claims;
bytes outside safe printable ASCII use canonical escapes.  Empty/hidden SSID
profiles, raw 64-hex PSKs, open networks, WPA3, WPA1, and enterprise
credentials are rejected by v1.

Each SSID appears at most once.  Duplicate decoded SSIDs are an error even if
their quoted spellings differ.  File order is policy order: later automatic
selection tries visible `auto` entries from first to last, with at most the
first four matches attempted by one `net wifi up`.  There is no hidden RSSI
priority.

## Frozen resource limits

- the complete file is at most 32 KiB (32768 bytes), including the header and
  final newline;
- one physical line is at most 512 bytes including its terminating newline;
- one file contains at most 64 decoded profiles; and
- the sum of decoded passphrase bytes in one model is at most 4096 bytes.

Readers enforce byte and count ceilings before allocating or decoding the next
item.  The 64-profile/4096-byte combination still admits 64 maximum-length
63-byte v1 passphrases.  Any limit excess rejects the complete generation; it
does not yield a prefix model.

The store is plaintext.  Quoting or `\xHH` is reversible encoding, not
encryption, hashing, key wrapping, or protection from a reader who can open the
file.

## `set-key` update semantics

```text
net wifi set-key SSID PASSPHRASE [auto]
```

- SSID and passphrase use the same decoded byte bounds as the file.  The CLI
  cannot represent embedded NUL.  Invalid syntax exits before opening the
  store.
- With `auto`, the resulting record is `auto`; without it, the resulting record
  is `manual`.  Updating without `auto` therefore clears a previous auto flag
  deliberately rather than retaining invisible state.
- An existing decoded SSID is replaced in place so profile order is stable.
  A new SSID appends after all existing records.
- The complete existing file is validated before modification.  An invalid
  file is preserved byte-for-byte and is not partially repaired by `set-key`.
- A missing file is created with the version header and one record.  No parent
  directories are created.
- The public argv grammar has the p002-documented process-list and shell-history
  exposure.  Diagnostics and the rewritten file path must not be described as
  eliminating that limitation.

Delete, rename, priority editing, importing legacy `/etc/wpa/`, interactive
prompting, raw-PSK entry, and profile listing are outside this Phase and need
separate commands if later required.

## Ownership, type, and mode rules

- `/etc/wifi.conf` is a regular file owned by UID 0, GID 0, mode exactly
  `0600`.
- A per-user `.wifi.conf` is a regular file owned by the calling effective UID,
  mode exactly `0600`; its group may be that account's primary group but grants
  no bits.
- The stable companion `.wifi.conf.lock` and every temporary file have the
  same owner policy and mode `0600`.
- Existing targets, locks, and validation files are opened with `O_NOFOLLOW`
  and checked with `fstat`.  Symlinks, non-regular files, unexpected owner,
  group-writable or world-readable bits, multiple-link targets, devices,
  FIFOs, and sockets are rejected.
- The selected home must be a directory reachable for the caller.  An unsafe
  or replaced final directory/target component fails closed; the writer does
  not follow a caller-controlled link after validation.
- Wrong ownership or broad mode is reported and left unchanged.  `set-key`
  does not silently `chown` or weaken a pre-existing object to make it usable.

These checks apply on every read as well as write.  A later `net wifi up` must
not consume a profile file which `set-key` would reject.

## Locking and atomic publication

Writers serialize on a persistent same-directory `.wifi.conf.lock` inode.
They never unlink that lock after use, because unlinking would permit two
callers to lock different inodes.  The initial creator uses `O_CREAT`,
`O_NOFOLLOW`, `O_CLOEXEC`, and mode `0600`; every opener verifies type, owner,
link count, and mode before acquiring a whole-file write lock.

Lock acquisition is interruptible and uses one five-second monotonic deadline.
Writers request the exclusive whole-file lock; every reader takes a shared lock
on the same persistent inode before opening and validating one complete target
generation, and holds it through the full bounded parse.  Timeout is an error;
no boot or interactive command waits forever and a retry starts a new command,
not an extension of the expired acquisition.

With the writer lock held, publication is ordered:

1. Open and validate the current target with `openat`/`O_NOFOLLOW`, or record
   that it is absent.
2. Read no more than the frozen maximum, parse the entire generation, apply the
   in-memory replace-or-append transaction, and serialize canonical v1.
3. Create a unique sibling temporary with `O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC`
   and mode `0600`; verify owner/type/mode after creation.
4. Check every write, synchronize the file, close it successfully, reopen or
   parse the exact staged bytes for validation, and reject any mismatch.
5. Rename within the already-open directory, synchronize that directory, then
   release the lock and all descriptors.

Before rename, every failure closes and removes only the transaction's unique
temporary and preserves the prior target exactly.  After successful rename, a
directory-sync failure, including an unsupported-directory-sync result,
reports an explicit durability-uncertain failure; it does not claim rollback
or overwrite the new complete file with an unchecked copy.  Errors preserve
their originating errno/stage across cleanup.

No writer uses truncate-in-place, a predictable shared temporary, an unlocked
read-modify-write, cross-directory rename, shell redirection, or a second
lock-file spelling.

## Memory and diagnostic redaction

- Passphrases are stored only in bounded mutable buffers needed for parse,
  update, serialization, and later selected-profile output.  Every copy is
  explicitly cleared before release on success and all failure paths.
- Parser errors identify version, record number, field class, or bounds but do
  not echo the record, quoted input, passphrase, PSK, or surrounding line.
- Writer, lock, fsync, and rename errors may name the fixed selected path and
  stage.  They never include credential contents.
- Every redacted diagnostic is at most 512 bytes including its terminator;
  truncation occurs only at field boundaries and carries an explicit marker.
- Temporary names, process argv, syslog, crash logs, analyzer output, and test
  evidence use dummy credentials.  Tests assert absence of each dummy secret
  from captured stdout/stderr and retained artifacts other than the expected
  protected fixture file.
- SSIDs are not authentication secrets, but generic failures need not echo
  them.  Tests and evidence use synthetic SSIDs rather than a real nearby
  network name.

## Non-goals

- encryption at rest, a keyring, TPM, hardware-backed secrets, or a desktop
  secret service;
- profile migration, merge, system/user inheritance, priority/RSSI policy, or
  multi-user session arbitration;
- contacting `networkd` during `set-key`, authenticating a socket peer, or
  implementing the later ZNV2 transport;
- scan, association, key installation in the driver, DHCP, route/DNS mutation,
  or reconnect; and
- adding credentials to `/etc/net.conf` or confirmed-commit rollback data.

## Ordered implementation packages

1. Encode the frozen 32-KiB file, 512-byte line, 64-profile, 4096-byte decoded
   secret, five-second lock, shared-reader, and 512-byte diagnostic limits in
   named constants and boundary test vectors.
2. Implement one bounded parser/model/canonical serializer shared by
   `set-key` and later `net wifi` readers; do not duplicate credential parsing
   in `networkd`.
3. Implement effective-UID/passwd path selection and directory-relative,
   no-follow ownership/type/mode validation.
4. Implement the persistent companion lock and checked same-directory atomic
   writer with failure injection at every open/read/write/sync/close/validate/
   rename/directory-sync boundary.
5. Add `net wifi set-key` dispatch, replace-in-place/append/auto semantics,
   zeroization, and redacted diagnostics without changing other `net` commands.
6. Add focused ordinary/sanitizer/analyzer fixtures and preserve wired
   network, netconf, build, and QEMU regressions.

## Verification plan

### Grammar and model

Cover an empty new store, multiple manual/auto records, every escape,
canonical round trip, replacement position, append order, auto clearing,
decoded duplicate SSIDs, 1/32/33-byte SSIDs, 7/8/63/64-byte passphrases,
missing/future version, missing newline, CRLF, tabs, NUL, truncated escape,
unknown security/mode/token, duplicate header, 511/512/513-byte physical
lines, 32767/32768/32769-byte files, 63/64/65 profiles, decoded-secret totals
through the valid v1 maximum of 4032 bytes, the bounded-model accumulator at
4095/4096/4097 bytes, and allocation failure.  No malformed input may produce
a partially accepted model.

### Path and permissions

Cover effective root, ordinary user, differing real/effective UID, `sudo`
semantics, ignored malicious `$HOME`, missing account/home, absent file,
root/user owners, wrong UID/GID/mode/type/link count, final symlink, lock
symlink, temporary collision, replaced directory entry, and inaccessible
home.  Inspect actual inodes and modes rather than mocking path strings.

### Atomicity and concurrency

Run concurrent same-SSID and different-SSID writers and readers.  Inject every
read, parse, lock, create, write, file-sync, close, staged-validation, rename,
directory-sync, cleanup, and unlock failure.  Every observer sees the complete
old or complete new generation; no lost unrelated record, unlocked update,
stale temporary, descriptor leak, or second lock inode is accepted.  Exercise
lock acquisition just below, at, and above the five-second deadline with a
monotonic-clock fixture, including interruption and waiter cancellation.

### Redaction and integration

Capture stdout, stderr, syslog hooks, temporary directory listing, and test
evidence for success and every failure.  The dummy passphrase may appear only
inside the expected mode-0600 target or staged fixture while owned by the test.
Prove `set-key` opens no networkd socket and invokes no `wifi`, `ifconfig`, or
`dhcpc` child.  Existing `net` commands and `/etc/net.conf` remain byte-for-byte
unchanged by the credential operation.

Run ordinary and ASan/UBSan variants where supported, the compiler analyzer,
existing WS011 netconf/persistence and WS002 network-service regressions,
`make -j16`, a bounded `qemu-system-x86_64` root/user file-ownership cell, and
`git diff --check`.  Do not use aggregate `make check` or `.internal/`.

## Acceptance conditions

- Root and non-root effective UIDs deterministically select only their fixed
  store, without trusting `$HOME`, request identity, or `networkd`.
- Every accepted file is strict canonical v1, regular, single-linked,
  correctly owned, mode `0600`, no larger than 32 KiB/512 bytes per line, and
  within the 64-profile/4096-secret-byte limits; unsafe files fail without
  modification.
- `set-key` replaces in place or appends exactly once, applies the explicit
  auto/manual result, and atomically publishes a complete validated generation
  under one persistent companion lock.
- Concurrent readers/writers and every injected failure observe a complete old
  or new file, retain the originating error, and leak no temporary, lock,
  descriptor, or secret buffer ownership.
- Readers share the persistent lock, writers acquire it exclusively, every
  acquisition ends within five seconds, and unsupported directory sync after
  rename returns durability-uncertain failure without a false rollback claim.
- No diagnostic, log, status, analyzer result, or retained evidence exposes a
  passphrase; every diagnostic stays within 512 bytes and the public argv
  exposure remains honestly documented.
- `set-key` does no network operation, `/etc/net.conf` contains no WLAN secret,
  and current wired `net`/`networkd`/`dhcpc`, build, and QEMU regressions pass.

## Frozen implementation parameters

The file/model, lock, reader, diagnostic, and post-rename durability behavior
are fixed above.  They are ordinary constants and fixture boundaries, not
manual gates and not permission to widen limits during implementation.

## Reconsideration boundary

Return to planning if the filesystem cannot provide one stable same-directory
lock plus atomic rename/durability semantics, if passwd-record home selection
cannot be made without following an unsafe path, or if later ZNV2 transport
would require `networkd` to open a client-selected credential pathname.  Do
not fall back to mode-broadened files, truncate-in-place, unlocked updates,
`$HOME`, request-supplied UIDs, `/etc/wpa/`, secrets in `net.conf`, or a
world-readable compatibility store.

## Queue boundary and handoff

q041 processed this Phase to the reconsideration boundary.  Its parser and
selected-profile API become dependencies of `ws005-p006`/`p007`, but the Phase
must be requeued after `ws001-p015` and `ws001-p016`; it does not itself
authorize socket protocol, association, DHCP, or physical-adapter work.

## q041 execution result

The host-side implementation is retained as safe partial progress:

- `wifi-conf 1` parsing, bounded counted-byte SSIDs, validation, transactional
  replacement/append, canonical serialization, limits, diagnostics, and
  explicit secret clearing are implemented in `wifi-conf.c`;
- the store selects root or passwd-record home from the effective UID without
  trusting `HOME`, walks absolute home components with `O_NOFOLLOW`, validates
  the final directory and every target/lock/temporary inode, and uses one
  persistent shared-reader/exclusive-writer lock with a strict five-second
  monotonic deadline;
- writers publish a validated same-directory temporary by rename, preserve the
  old generation before rename, report post-rename durability uncertainty,
  never unlink the published lock inode, and sanitize a secret-bearing
  temporary before reporting an unremovable residual; and
- `/sbin/net wifi set-key SSID PASSPHRASE [auto]` is local-only and clears its
  mutable argv passphrase after use.  It does not contact `networkd` or alter
  `/etc/net.conf`.

The Phase-owned ordinary, ASan+UBSan, compiler-analyzer, and parser/model gates
pass.  Existing WS011 console, boot-application, parser, and persistence gates;
the WS005 userland recovery and networkd-authentication gates; `make -j16` for
the configured PC-98 tree; and `git diff --check` also pass.  The sandboxed
AF_UNIX listener was denied as expected, and the identical bounded boot test
passed when run with local socket permission.

Completion is intentionally not claimed.  Source audit proved that native
`O_CREAT` does not carry effective ownership into UFS/overlay creation and that
directory `fsync()` may return a false success.  Consequently the required
root/non-root native ownership and remount-durability cell cannot be honest.
Resume only after both WS001 prerequisites complete, then rerun the full guest
cell and all retained host regressions from the same artifact.

# Security design

Status: design (2026-10-05). This is the target design: the document comes
first and the implementation follows it, so some of what it describes is not
built yet.

This document records where zedBSD and Keiland draw their privilege
boundaries and why. It covers account administration and the authentication
of the graphical login. Later sections will cover the other privileged paths.

## Principles

- **The desktop holds no privilege.** The compositor, Settings and the other
  Keiland programs run as the logged-in user. An action that needs root goes
  through a small privileged program whose whole job is that action, and that
  program checks the request itself. It never trusts the caller.
- **A privileged program is small and has one job.** It reads a fixed request
  format, checks the caller and the request, does the work and exits. There
  is no general command channel and no shell.
- **The administrator proves presence.** An administrative change needs the
  administrator's own password at the time of the change, the same as `sudo`.
  A logged-in session alone is not enough, because an unlocked, unattended
  session must not be able to create accounts.
- **Administrators are the wheel group.** A user is an administrator when
  `wheel` is the user's primary group or names the user. root is not managed
  from the desktop.
- **Every privileged request is logged** to syslog's `auth` facility with the
  caller, the operation and the result. Passwords and hashes are never logged.
- **System files are replaced atomically.** A reader sees the old file or the
  new one, never a part (see [Shared account core](#shared-account-core)).

## Account administration

### What it covers

An administrator can do the following from Settings > Users:

- add a user, with a name, a display name, a password and an administrator
  switch;
- remove a user, keeping the home directory by default;
- reset another user's password;
- add a user to or remove a user from the `wheel` and `network` groups.

Every user can see the list of users. Every user can change their own
password; that path is separate (`passwd -s`, below).

### The path

```text
Settings (user)  ->  compositor (user)  ->  libkeiland-backend (user)
                                              |
                                              | runs as a child, request on stdin
                                              v
                                       account-admin (set-user-ID root)
                                              |
                                              v
                              /etc/passwd, /etc/shadow, /etc/group, /home
```

- Settings asks for the operation through a `kl_system_account_*` request on
  the compositor's system extension. Settings itself has no privilege and
  never runs the tool.
- The compositor's operating-system backend starts `account-admin` as a
  child, writes one request to its standard input, reads one answer from its
  standard output and waits for it to exit. This is the same pattern as the
  user's own password change through `passwd -s`.
- `account-admin` is the only privileged part. It is a set-user-ID root
  program in the base system (source `userland/base/account-admin/`),
  installed as `/usr/libexec/account-admin`, mode 4555, owned by root. It is
  not on any user's `PATH`, because no one runs it by hand.

The tool was chosen over a new request on `sessiond`, the root session
daemon. `sessiond` could identify the caller from its socket and skip the
password, but each new request widens the interface of a long-running root
daemon. A short-lived tool that checks the password and exits keeps the
privileged surface small, can be audited on its own, and follows the same
rule as `sudo`: the administrator types their password for each change.

### Request format

The request is text on standard input, one field per line, ending at end of
file:

```text
<caller's password>
<operation>
<arguments, one per line>
```

| Operation | Arguments |
| --- | --- |
| `add` | name, display name, new password, `admin` or `user` |
| `remove` | name, `keep-home` or `remove-home` |
| `reset-password` | name, new password |
| `group-add` | name, group (`wheel` or `network`) |
| `group-remove` | name, group (`wheel` or `network`) |

Nothing comes from the command line or the environment. The tool ignores
`argv` beyond its name and clears its environment before it starts.

The answer is one line on standard output, `ok` or `error <reason>`, and the
exit status is 0 or 1. The reasons are fixed words, such as
`not-administrator`, `bad-password`, `no-such-user`, `name-taken`,
`bad-name`, `weak-password`, `last-administrator`, `self`, `root`, `busy` and
`home-exists`,
so that Settings can show a message in the user's language.

### Checks, in order

1. **The caller.** The caller is the real user ID. The tool looks it up in
   `/etc/passwd`. A real user ID of 0 is refused, because root administers
   with the base tools, not this path.
2. **Administrator.** The caller must be in `wheel`. Otherwise the answer is
   `not-administrator`, and the password is not checked, so the tool cannot
   be used to test passwords of non-administrators.
3. **Password.** The caller's password must match the hash in
   `/etc/shadow`. A wrong password is answered after a fixed delay of 2
   seconds, and the failure is logged.
4. **The target.**
   - root and system accounts (user ID below 1000) are refused (`root`).
   - An administrator cannot remove themselves or take themselves out of
     `wheel` (`self`).
   - The last administrator among the people's accounts (user ID 1000 and
     up) cannot be removed or taken out of `wheel` (`last-administrator`).
     root does not count: it is always in `wheel` but is locked in a release
     build, so the machine always keeps an administrator who can log in.
5. **The values.**
   - A new name is a lower-case letter followed by up to 31 lower-case
     letters, digits, `-` or `_`, and must not be taken (`bad-name`,
     `name-taken`).
   - A display name may not contain `:` or a newline.
   - A new password follows the shared password rules (at least 8
     printable characters).
   - The group is `wheel` or `network`; no other group can be changed
     here.

Only after every check passes does the tool change anything.

### What each operation changes

- **add:** takes the lowest number from 1000 up that is free both as a user
  ID and as a group ID, and gives the user a private group of the same name
  and number. Writes the `/etc/passwd`, `/etc/group` and
  `/etc/shadow` lines (SHA-512 crypt), and adds the user to `wheel` when
  asked. Creates `/home/<name>` with mode 0700, owned by the new user, and
  copies the skeleton files from `/etc/skel` into it, when there are any:
  only the regular files directly in it, owned by the new user, without
  group and other permissions. If `/home/<name>` already exists (for
  example kept from an earlier removal), the request is refused
  (`home-exists`) and the directory is left alone, so an old home's files
  never pass silently to a new account.
- **remove:** removes the user's lines from `/etc/passkey`, the three
  account files and every group's member list. The home directory is kept unless `remove-home` was
  given. It is removed only after Settings has asked the administrator to
  confirm, and the tool never follows a symbolic link while removing it.
  A user who has a running process (logged in, or a program left running)
  cannot be removed (`busy`). The tool looks for any process with that
  user's ID in the kernel's process list, and for a login record naming
  them.
- **reset-password:** replaces the user's hash, and removes the user's PIN
  and security keys from `/etc/passkey`. The user is not told the old one,
  and no one can read it.
- **group-add / group-remove:** edits the member list of `wheel` or
  `network` in `/etc/group`.

### Robustness

- All four files are changed under the shared lock, in the order `passkey`
  (removing any line left for the name), `group`, `passwd`, `shadow` for an
  addition, and `passkey`, `shadow`, `passwd`, `group` for a removal. Each is
  replaced atomically. A crash between files leaves at most an unused group
  or an account without a password entry, which cannot log in, and never a
  PIN or security key that a later account of the same name inherits.
- Signals that would stop the tool midway are held during the change.
- The tool reads a bounded request (4 KiB). Longer input is refused before
  any check.
- Password buffers are cleared before the tool exits.

### Other operating systems

On Linux and FreeBSD the Keiland ports show the list of users, but the
administrative operations answer "not supported here". Those systems have
their own account tools (AccountsService, `pw`), and wrapping them is a
separate design.

## Login authentication

### What it covers

The graphical login screen and the lock screen accept three kinds of
credential, called styles:

- **password**: the account's password, as everywhere else;
- **pin**: a six-digit PIN the user sets in Settings > Users;
- **fido2**: a FIDO2 security key the user registers in Settings > Users,
  touched (or tapped on an NFC reader) and unlocked with the key's own PIN.

The PIN and the security key are conveniences for the person at the machine.
They are never accepted by `login` on the console, `su`, `sudo`, `passwd` or
SSH, which take the password only. They are offered only for the people's
accounts (user ID 1000 and up), never for root or a system account, and only
while the account's password is neither locked nor expired.

### The parts

```text
greeter / lock screen (no privilege)
        |  one request per line on its socket pair
        v
sessiond (root, resident)  -- chooses the style, counts failures, delays
        |  request on stdin, answer on stdout
        v
/sbin/passkey (root, short-lived)  -- password and PIN; /etc/passkey
        |  the same request format
        v
/usr/libexec/passkey-fido2 (root, short-lived)  -- security keys: the challenge and the check
        |  the device descriptors only
        v
device helper (_passkey, chroot /var/empty)  -- talks CTAP to the security key
```

- **sessiond** never checks a credential itself. For each attempt it starts
  `/sbin/passkey` in a process group of its own, writes the request to its
  standard input and reads the answer from its standard output, while its
  own loop keeps serving the session (the answer is a descriptor it polls,
  not a wait). It keeps the failure counts and applies the delays. The
  account it names is the one the greeter chose for a login, and always the
  session's own user for an unlock or a change.
- **`/sbin/passkey`** is a base program, mode 0500, owned by root and not
  set-user-ID. It refuses to run unless its real user ID is 0. It reads one
  bounded request (4 KiB), does that one thing and exits. Nothing comes from
  its command line or its environment. It checks passwords and PINs with
  the C library's `crypt()` alone, so a password login never depends on
  anything outside the base system.
- **`/usr/libexec/passkey-fido2`** is passkey's companion for the security
  key style, started by passkey with the same request. It is the only part
  that links the cryptography library a security key needs. It makes the
  challenge, starts the device helper, and checks the key's answer itself.
- **The device helper** is a child of passkey-fido2. It runs as the
  `_passkey` account inside an empty root directory, may open no files and
  start no processes, and holds only the descriptors of the security keys'
  device nodes, which passkey-fido2 opened, and of its two pipes. It sends
  the keys their requests and returns the chosen key's answer as bytes. It
  does not read `/etc/passkey`, does not choose the challenge and does not
  decide whether the answer is good. It ends when its parent's pipe closes.

### The request

The request is text, one field per line; every field ends with a line end,
and each operation has an exact number of fields. A field may not contain a
NUL or another control character. Anything else is answered `bad-request`.

```text
<operation>
<account name>
<style>             (auth only)
<secret>            (auth: the password, the PIN, or the key's PIN;
                     the other operations but styles and enrolled: the user's current password)
<arguments>
```

| Operation | Arguments |
| --- | --- |
| `auth` | none |
| `styles` | none (and no secret): the styles the account has enrolled and may use now |
| `enrolled` | none (and no secret): the account's PIN and keys, without secrets: `pin=0|1 fido2=N`, then `key=REF/LABEL` for each key (its reference and its label's bytes in hexadecimal) |
| `enroll-pin` | the new PIN |
| `remove-pin` | none |
| `enroll-fido2` | a label, the key's PIN |
| `remove-fido2` | the credential's ID, or its reference |

The answer is zero or more `status touch` lines (the user should touch or
tap the key), then `ok uid=<user ID>` (with `id=<credential ID>` after
`enroll-fido2`) or `fail <reason>`. A key's reference is the 64-bit FNV-1a hash
of its credential ID's base64url text in 16 hexadecimal digits: a short name
for one of an account's few keys, not a secret. sessiond compares the user ID with the
account it asked about. The reasons are fixed words: `bad-secret`,
`no-such-user`, `not-enrolled`, `locked-account`, `pin-off`, `no-key`,
`many-keys`, `key-locked`, `timeout`, `device`, `cloned`, `bad-request`,
`busy` and `internal`. The exit status is 0 for `ok`, 1 for `fail` and 2 for
an internal error. sessiond tells a passkey that ends without an answer as
`internal` (it logs how it ended), and refuses at once with `internal`,
without counting an attempt, when `/sbin/passkey` is not there; `timeout` is
only an attempt sessiond stopped. Every image with sessiond has passkey,
because the sessiond package requires it.

passkey keeps its own deadline: 5 seconds for a password or a PIN, 30
seconds for the user to touch a key. sessiond allows 5 seconds more, then
sends the process group `SIGTERM`, and `SIGKILL` 2 seconds later. passkey
holds its signals while it rewrites `/etc/passkey`. The greeter and the lock
screen can stop a security key attempt with `CANCEL`; passkey then cancels
the key's request and answers `fail timeout`.

### Failure counts and delays

sessiond counts the failures in a row for each account (by user ID; the
unknown names share one count) and style in memory. They are never written
to a file, so that an attempt leaves no trace an attacker could time or
watch. An attempt is counted before passkey is started and the count is
cleared only by a success, so a crash or a timeout in the middle of an
attempt counts as a failure. After a failure the answer waits: 2 seconds,
doubling after every three failures in a row, up to 16 seconds, for every
style of the account together. A wrong password given to set or remove a
PIN or a key counts the same way.

The PIN is offered for an account only after that account has logged in or
unlocked with its password or a security key, or proved its password to set
or remove its PIN or a key, since sessiond started, like a phone that asks for
its passcode after a restart. So a restart, which anyone
at the login screen can cause, gives no new PIN attempts. After five wrong
PINs in a row the PIN is turned off again until the next password or
security key login. A security key counts its own wrong PINs and locks
itself after eight.

### /etc/passkey

The enrolled credentials live in `/etc/passkey`, owned by root with mode
0600, beside `/etc/passwd` and `/etc/shadow`, which do not change. passkey
refuses the PIN and security key styles (not the password) while the file is
owned by anyone else or readable by anyone else. It is rewritten atomically
under the shared account lock, read again under that lock for every change,
like `/etc/shadow`.

```text
# zedBSD passkey 1
<name>:<uid>:pin:<SHA-512 crypt hash>
<name>:<uid>:fido2:<credential ID>:<COSE public key>:<signature count>:<relying party>:<label>:<date>
```

A line counts only while both its name and its user ID match the account in
`/etc/passwd`. The credential ID and the key are base64url. An account has at
most one PIN and five security keys; a label has at most 32 characters, none
of them a control character or `:`. A line of a kind passkey does not know is
kept as it is when the file is rewritten; a file whose first line names a
later version is only read. A malformed line is ignored and logged once.

When an account is removed, its lines go first, before its `/etc/shadow`,
`/etc/passwd` and `/etc/group` entries; adding an account removes any line
left for its name. Resetting a user's password through account
administration removes the user's PIN and security keys too, since a reset
often follows a lost or misused account.

### The PIN

A PIN is exactly six decimal digits, hashed like a password (SHA-512 crypt).
Setting, changing or removing it requires the account's current password. An
account whose password is locked cannot have a PIN. Six digits give a
million combinations: the protection is the file's permissions, the limit of
five attempts and the rule that a restart gives no new ones, not the hash.

### The security key

The relying party is `zedbsd.login`, a name that cannot collide with a web
site's. Only ES256 credentials (COSE algorithm -7, P-256) are used, always
with the key's own user verification (its PIN, or its built-in
verification). A key without a PIN of its own cannot be registered or used.

**Choosing the key.** The key's PIN goes to one key only, never to every key
that is plugged in: a key that does not know the PIN would count it as a
wrong one, and a rogue device would learn it. passkey-fido2 first asks every
key present, without user presence or verification, whether it holds one of
the account's credentials. When one key does, that key is used; when several
do, the user touches the one to use (the key's selection command). Only then
does the helper agree a PIN/UV secret with that key, send it the PIN and ask
for the assertion. A key that holds none of the credentials answers `no-key`
without ever seeing the PIN. Keys plugged in or tapped during the attempt are
asked too.

**A key held to an NFC reader.** passkey-fido2 also opens every smart card
slot (`/dev/smartcardN`) and hands them to the helper, which powers a card
and selects its FIDO applet only when it asks it; a slot whose card does not
answer (a reader's SAM slot, a card that is not a security key) is let go and
not counted. For an NFC key, being in the reader's field is the user's
presence, and a card left lying on the reader counts too (the user's decision
of 2026-10-10): a login asks the cards already on a reader after the USB
keys, and when no key answers at all, the screen asks to touch the key or hold
it to the reader and the helper waits for a card until the touch's time is
nearly out (`timeout` when none came). So a key left on the reader is like a
key that is touched: with the key's PIN not required, anyone at the machine
signs in or unlocks with it, which Settings says when that choice is made.
Registering counts a card already on the reader as the one key (two keys, USB
or NFC, are refused), and with no key at all waits for one to be held there
the same way.

**Registering.** Registration asks for exactly one security key: with two or
more present it is refused (`many-keys`), so a rogue device cannot slip in
its own credential. The settings page shows the key's name and place before
the user touches it. The key makes a new non-resident ES256 credential with
user verification. passkey-fido2 reads the key's answer itself: the
relying-party hash, the user-present, user-verified and attested-data flags,
the credential ID in the attested data, and an ES256 public key on the curve.
The key's attestation statement is not checked, so the make of the key is
not part of the trust: what registration protects against is a credential
the user did not create on a key they hold, not a counterfeit key.

**How a key signs in.** An account with a key chooses on Settings' Security
Keys page how it signs in (a line of /etc/passkey,
`name:uid:options:methods=...:key-pin=0|1:key-touch=0|1`, which WS200's
Sign-in Methods shares; none, several or one that does not read are the
defaults): the key's PIN and a touch (the default), a touch alone, or
neither to unlock. The login screen always asks for a touch; only an unlock
may go without one, and only after the lock screen's swipe. Without the
PIN the key does not verify the user, so anyone who holds the key signs in
with a touch; and a key left plugged in, or lying on an NFC reader (being
in the field counts as a touch), signs in or unlocks for anyone at the
machine. Settings says so, and asks the password, before a weaker choice.
sessiond sends a key's login as `auth-fido2 NAME login PIN` and an unlock
as `... unlock PIN`, and passkey-fido2 reads the account's line: an empty
PIN only when the PIN is not asked, no touch only for an unlock when the
touch is not asked, and the flags it checks in the answer follow. A key's
login or unlock counts as a wrong attempt only when the key's PIN was wrong,
the key is a clone or its answer does not verify; a key that was not there,
not touched or taken away is not counted and is answered at once (the key
counts its own wrong PINs). When an account's last key goes, its key's
choice goes back to the default.

**The key's own operations.** Settings' Security Keys page also asks what
key is there, sets or changes the key's PIN, and resets the key, through
sessiond's `KEYINFO`, `KEYPIN set|change` and `KEYRESET` (a session's only;
the greeter cannot ask them) and passkey's `key-info`, `key-set-pin`,
`key-change-pin` and `key-reset`, which passkey-fido2 carries out as
above. What the key is (how many keys, a key's name, whether it has a PIN,
its retries) is told without any secret. Its PIN is checked by the key
alone, which counts the wrong ones itself (eight in all, three per power
cycle), so `KEYINFO` and `KEYPIN` are not attempts of the account: they
neither count, clear the counts, delay nor offer the six-digit PIN. A reset
erases every credential and the PIN on the key, so it asks the account's
password, counted as a password attempt; once passkey-fido2 says the
password was right (`status verified`) the counts are cleared as by a
password, and later failures are told at once. The key takes a reset only a
few seconds after it is powered, so passkey-fido2 asks the user to plug it
in again (`status replug`, or to take it from the reader and hold it there
again), sends the reset as soon as the key comes back, after looking within
a short time for which of this machine's credentials (of every account) it
held, and removes their lines once the key says it is reset. A reset under
way is cancelled with the key (CTAPHID_CANCEL) when sessiond ends the
request (Cancel, the deadline of 75 seconds, the screen locking or the
machine going to sleep): passkey ignores sessiond's SIGTERM and waits for
passkey-fido2, which passes it to the helper and waits for the key's answer.
Settings is told passkey's own word for a change (`bad-key-pin`,
`key-locked`, `key-replug`, `no-pin`, `pin-policy`, `not-allowed`, ...);
the login and lock screens are told the greeter's words as before.

**Logging in.** passkey-fido2 makes a random 32-byte challenge and the client
data hash `SHA-256("zedbsd.login" NUL name NUL challenge)`. The helper returns
the chosen key's answer, and passkey-fido2 then:

1. finds the public key by the credential ID in the answer, among the
   account's own registrations;
2. checks that the authenticator data's relying party hash is the hash of
   `zedbsd.login`, that its flags say the user was present and verified, and
   that it carries no attested data and nothing after its extensions;
3. checks the signature over the authenticator data and its own client data
   hash;
4. checks the signature count: when the stored count or the new one is not 0,
   the new one must be larger (otherwise `cloned`: the key may have been
   copied). A larger count is stored; the stored count never goes down.

The security keys' device nodes are opened by passkey-fido2 alone while it
runs, and claimed for that time so that no other program can send the key a
request the user's touch would answer. They are never given to the login
screen. On an NFC reader only the contactless slot is used, and a slot
another program holds is left alone.

## Shared account core

`passwd`, `su`, `sudo` and `account-admin` share one implementation of the
account rules (`userland/base/common/account.c`):

- **Passwords:** at least 8 printable characters when a user chooses one,
  and different from the one they replace. Hashes are SHA-512 crypt with a
  high round count and 16 random characters of salt from `getentropy`.
- **File replacement:** `/etc/shadow` and the other files are never written
  in place. Under an exclusive lock file, the whole file is read, the change
  is applied, the result is written to a new file in `/etc` with the
  original's mode, synced, and renamed over the original. Then `/etc` is
  synced.
- **wheel:** a user is in `wheel` when it is the primary group or the group
  names the user.
- **Environment:** a command run as another user keeps only the terminal and
  locale variables of the caller and sets `HOME`, `SHELL`, `USER`, `LOGNAME`
  and a fixed `PATH`. `LD_*`, `IFS`, `ENV` and the rest never pass through.

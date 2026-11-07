# Security design

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
  switch; the new user is also put in the `video` group, whose members may
  open the GPU and so start the desktop from a console login (`useradd`
  puts only the groups it is given);
- remove a user, keeping the home directory by default;
- reset another user's password;
- add a user to or remove a user from the `wheel` and `network` groups.

root can do the same and a little more from a terminal with `useradd`,
`usermod` and `userdel`: set the shell, the display name, the user ID of a
new user and the primary group, replace or add to the groups, lock and
unlock the password, and remove a user that still has programs running.

Every user can see the list of users. Every user can change their own
password; that path is separate (`passwd -s`, below).

### The path

```text
Settings (user)  ->  compositor (user)  ->  libkeiland-backend (user)  --+
                                                                         | runs as a child,
useradd / usermod / userdel (root, in a terminal or through sudo)  ------+ request on stdin
                                                                         v
                                                      account-admin (set-user-ID root)
                                                                         |
                                                                         v
                                   /etc/passwd, /etc/shadow, /etc/group, /etc/passkey, /home
```

- Settings asks for the operation through a `kl_system_account_*` request on
  the compositor's system extension. Settings itself has no privilege and
  never runs the tool.
- The compositor's operating-system backend starts `account-admin` as a
  child, writes one request to its standard input, reads one answer from its
  standard output and waits for it to exit. This is the same pattern as the
  user's own password change through `passwd -s`.
- `useradd`, `usermod` and `userdel` are front ends that only root runs.
  They turn their options into one request and run `account-admin` the same
  way. They read and write no account file themselves. Their options and
  exit statuses are in [the reference](../reference/user-administration.md).
- `account-admin` is the only part that changes the account files. It is a
  set-user-ID root program in the base system (source
  `userland/base/account-admin/`), installed as
  `/usr/libexec/account-admin`, mode 4555, owned by root. It is not on any
  user's `PATH`, because no one runs it by hand. Keeping every edit, check
  and lock in one program means Settings and the terminal tools follow the
  same rules.

The tool was chosen over a new request on `sessiond`, the root session
daemon. `sessiond` could identify the caller from its socket and skip the
password, but each new request widens the interface of a long-running root
daemon. A short-lived tool that checks the password and exits keeps the
privileged surface small, can be audited on its own, and follows the same
rule as `sudo`: the administrator types their password for each change.

### Request format

The request is text on standard input, one field per line, ending at end of
file. Its form depends on the caller's real user ID.

A caller that is not root (Settings) gives its password first:

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
| `system-language` | `en` or `ja` (the login screen's language) |

root (real user ID 0) gives no password. The first line is the operation,
and every other line is `KEY VALUE`: the key, one space, and the value to
the end of the line (it may be empty). A key may appear once.

| Operation | Keys (`name` is required) | Front end |
| --- | --- | --- |
| `user-add` | `name`, `comment`, `shell`, `uid`, `group`, `groups`, `home` (`make` or `none`) | `useradd` |
| `user-modify` | `name`, `comment`, `shell`, `group`, `groups` or `groups-add`, `password` (`lock` or `unlock`) | `usermod` |
| `user-remove` | `name`, `home` (`keep` or `remove`), `force` (`yes`) | `userdel` |

- `comment` is the display name. `shell` is an absolute path to a program
  that exists. `uid` is from 1000 to 59999 and is not used by any user or
  group. `group` (the primary group) is `wheel`, `network` or the user's own
  group. `groups` and `groups-add` are lists of `wheel` and `network`
  separated by `,`. `groups` replaces the user's groups (an empty list takes
  the user out of all of them); `groups-add` adds to them.
- A `user-modify` must change something. `home remove` and `force yes`
  cannot be given together, because a running program could keep writing in
  the home while it is removed.
- root cannot use Settings' operations, and other callers cannot use root's.

Nothing comes from the command line. The tool ignores `argv` beyond its
name and clears its environment before it starts. The one exception is
`SUDO_USER` when the caller is root: it is read only to be written to the
log, because anyone can set it.

The answer is one line on standard output, `ok` or `error <reason>`, and the
exit status is 0 or 1. The reasons are fixed words, such as
`not-administrator`, `bad-password`, `no-such-user`, `name-taken`,
`bad-name`, `weak-password`, `last-administrator`, `self`, `root`, `busy`,
`home-exists`, `failed`, `bad-request`, `bad-value` (a key's value outside
its rules), `id-taken`, `no-password` (an unlock with no password behind
the lock) and `home-failed` (the account was made but its home could not
be), so that Settings and the front ends can show a message in the user's
language.

### Checks, in order

1. **The caller.** The caller is the real user ID.
   - root is accepted without a password: it can already edit the files.
   - Any other caller is looked up in `/etc/passwd` and must be in
     `wheel`. Otherwise the answer is `not-administrator`, and the password
     is not checked, so the tool cannot be used to test passwords of
     non-administrators.
   - That caller's password must match the hash in `/etc/shadow`. A wrong
     password is answered after a fixed delay of 2 seconds, and the failure
     is logged.
2. **The target.**
   - root and system accounts (user ID below 1000, or above 59999) are
     refused (`root`).
   - In Settings, an administrator cannot remove themselves or take
     themselves out of `wheel` (`self`). root's front ends have no such
     check, because under `sudo` the tool cannot know reliably who is
     typing; the next rule protects the machine instead.
   - A user who has a running process cannot be removed (`busy`), unless
     root forces it.
3. **The values.**
   - A new name is a lower-case letter followed by up to 31 lower-case
     letters, digits, `-` or `_`, and must not be taken as a user or a group
     (`bad-name`, `name-taken`).
   - A display name may not contain `:` or a control character.
   - A new password from Settings follows the shared password rules (at
     least 8 printable characters).
   - root's keys follow the rules under [Request format](#request-format).
4. **The last administrator.** The tool makes the new texts of all three
   files in memory first. If the files had at least one administrator who
   can log in and the new texts have none, the change is refused
   (`last-administrator`). This one rule covers every way to lose the last
   one: removing them, locking their password, giving them a shell that
   does not log in, moving their primary group off `wheel`, or replacing
   their groups. An administrator who can log in is a user with an ID from
   1000 to 59999 who:
   - is in `wheel` (as the primary group or by name);
   - has a shell that logs in (no shell, or one that is not a `nologin` or
     `false`);
   - has a shadow line whose hash is not empty and does not start with `!`
     or `*`;
   - has an account that has not expired (the shadow's expiry field is
     empty, 0, or a later day).

   root does not count. It is always in `wheel`, but its password is locked
   in a release build, so the machine must keep an administrator who can log
   in. A user that `useradd` made without a password does not count either.
   A machine that had no such administrator before the change is not refused,
   because the change cannot make it worse.

Only after every check passes does the tool change anything.

### What each operation changes

- **add / user-add:** gives the user a new user ID and a private group of
  the same name and number (always, even when another primary group is
  chosen), writes the `/etc/passwd`, `/etc/group` and `/etc/shadow` lines,
  and adds the user to the groups asked for. Settings writes the password's
  hash (SHA-512 crypt). `useradd` writes `!`, a locked password, and the
  password is set later with `passwd NAME`. The home is always
  `/home/<name>`. Unless `home none` is given, the tool creates it with mode
  0700, owned by the new user and their primary group, and copies the
  skeleton files from `/etc/skel` into it: only the regular files directly
  in it, owned by the new user, without group and other permissions.
  Directories in the skeleton are not copied. If `/home/<name>` already
  exists (for example kept from an earlier removal), the request is refused
  (`home-exists`) and the directory is left alone, so an old home's files
  never pass silently to a new account.
- **The new user ID** is one more than the highest of all of these, so an ID
  is never given twice while anything of its last owner may be left:
  - every user ID and group ID from 1000 to 59999 in the files;
  - the record of the highest ID ever given (`/etc/account-admin.last-id`,
    written before the account files);
  - the owners of the entries in `/home`;
  - cron's tables, named by user ID;
  - the owners of at's jobs.

  root may give the ID with `uid` instead.
- **remove / user-remove:** removes the user's lines from `/etc/passkey`,
  `/etc/shadow` and `/etc/passwd`, and the user's name from every group's
  member list. The user's own group is removed too, unless it is another
  user's primary group or has other members; such a group is kept, and the
  log says so. After the change, the tool removes the user's cron table and
  at jobs, and removes `/etc/keiland/autologin` when it names the user, so a
  later account of the same name is not logged in by itself. The home
  directory is kept unless `remove-home` (or `home remove`) was given. It is
  removed only when it is `/home/<name>`, the user's home in `/etc/passwd`,
  and a directory the user owns. In Settings, the administrator confirms it
  first. While removing it, the tool never follows a symbolic link and
  never enters another file system mounted inside it. What it leaves is
  said in the log. To find a running process (`busy`), the tool looks for
  any process with that user's ID in the kernel's process list, and for a
  login record naming them.
- **Leftovers:** if a crash left lines of a name without its `/etc/passwd`
  line, removing that name cleans its shadow line, its own group, its name
  in the member lists and its `/etc/passkey` lines. The answer is
  `no-such-user` only when nothing of the name is left.
- **reset-password:** replaces the user's hash, and removes the user's PIN
  and security keys from `/etc/passkey`. The user is not told the old one,
  and no one can read it.
- **group-add / group-remove:** edits the member list of `wheel` or
  `network` in `/etc/group`.
- **user-modify:** sets the display name (`comment`), the shell, or the
  primary group in `/etc/passwd`, and edits the member lists in
  `/etc/group`. `password lock` puts a `!` in front of the hash. `password
  unlock` takes it off, but is refused (`no-password`) when nothing that
  can be typed is behind it: an empty hash would let anyone log in with an
  empty password.
- **root's `passwd NAME`** is not part of this tool. It replaces the whole
  hash field, so it also unlocks a locked password. It does not remove the
  user's PIN and security keys. To revoke those as well, use Settings'
  password reset.
- **Locked accounts and automatic login:** `sessiond` does not log in the
  account named by `/etc/keiland/autologin` when its shadow line is
  missing, its password is locked (`!` or `*` in front of the hash), or the
  account has expired. It shows the login screen instead.

### Robustness

- The account files are changed under the shared lock (see
  [Shared account core](#shared-account-core)). Under the lock, the tool
  writes only the files: the record of the last user ID, `/etc/passkey`,
  then each account file that changed, replaced atomically. The order is
  `group`, `passwd`, `shadow` for an addition, and `shadow`, `passwd`,
  `group` for any other change. A crash between files leaves at most an
  unused group or an account without a password entry, which cannot log in.
  It never leaves a PIN or security key that a later account of the same
  name inherits.
- If a file cannot be written, the files written before it are written back
  from the texts read under the lock, and the answer is `failed`.
- What can take long happens after the lock is released, so other tools
  that wait for the lock are not held: creating a home, copying the skeleton,
  removing a home, and cleaning the job spools and the automatic login.
- Signals that would stop the tool midway are held during the change.
- The tool reads a bounded request (4 KiB). Longer input is refused before
  any check.
- Password buffers are cleared before the tool exits.
- Every request's result goes to the system log (auth), with the caller.
  For root, the log also gives `SUDO_USER` when it is set. Passwords and
  hashes are never logged.

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
- **pin**: a PIN of four to six digits; a person's new account has the PIN
  0000, which the user changes in Settings > Users;
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
unknown names share one count) and style in memory. Only the wrong PINs in a
row are also kept in a file, `/var/db/sessiond/pin-tries` (root's alone, mode
0600 in a directory of mode 0700, one line `UID TRIES` for each account with
some), written before passkey is started; the other counts are never written,
so that an attempt leaves as little trace as it can. An attempt is counted before passkey is started and the count is
cleared only by a success, so a crash or a timeout in the middle of an
attempt counts as a failure. After a failure the answer waits: 2 seconds,
doubling after every three failures in a row, up to 16 seconds, for every
style of the account together. A wrong password given to set or remove a
PIN or a key counts the same way.

The PIN is offered from the start, before any password: a tablet without a
keyboard logs in and unlocks with the PIN on the screen's keypad, so a rule
that asked for the password first after a restart would lock it out. After
five wrong PINs in a row the PIN is turned off until the account logs in or
unlocks with its password or a security key, or proves its password to set
or remove its PIN or a key. Because the wrong PINs are kept in their file, a
restart, which anyone at the login screen can cause, gives no new PIN
attempts. A security key counts its own wrong PINs and locks
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
left for its name and, for a person's account, adds the PIN 0000. Resetting a user's password through account
administration removes the user's PIN and security keys too, since a reset
often follows a lost or misused account.

### The PIN

A PIN is four to six decimal digits, hashed like a password (SHA-512 crypt).
Setting, changing or removing it requires the account's current password. An
account whose password is locked cannot have a PIN.

A person's account is given the PIN 0000 when it is made (Settings' or
root's account administration, and the default user of an image), so that a
tablet without a keyboard can log in before anyone has set a PIN; the user
changes it in Settings. This is weaker than a PIN the user chose: anyone who
knows the default can log in to an account whose PIN was never changed.
Four digits give ten thousand combinations and six a million: the
protection is the file's permissions, the limit of five attempts and the
file of wrong attempts that a restart does not clear, not the hash.

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

**Sign-in methods.** The user chooses on Settings' Users page which of the
password, the PIN and a security key the login and lock screens take (the
same options line's `methods=password,pin,fido2` or part of it; `set-methods
NAME PASSWORD METHODS`, checked by the account's password, through sessiond's
`SETMETHODS`). A method takes effect only while it is set up (a PIN, a key
registered); when none of the methods that take effect is the password or a
key, the password takes effect too, because the PIN alone is never the first
sign-in after a start. So the methods can never shut an account out of the
screens, and `set-methods` refuses methods without the password or a key, and
without the password unless the account has a key. passkey answers `styles`
with the methods that take effect, and refuses `auth` in a method the account
turned off with `style-off` before it looks at the secret (passkey-fido2 does
the same for a key, and leaves such an account out of a key's owners). The
console, `su`, `sudo`, `passwd` and SSH do not ask passkey and always take the
password, which is how a user who turned the password off and lost the key
gets back in. `enrolled` tells the methods as bits after `key-touch=`
(`methods=N`: 1 the password, 2 the PIN, 4 a key).

**The key's owner.** When a key is plugged in or held to a reader while the
login screen shows (or while the lock screen's card shows, after a swipe),
the screen asks sessiond whose it is (`KEYOWNER`; passkey's
`key-owner NAME|-`). passkey-fido2 asks the one key there, silently (no
touch, no PIN), which account's registrations it holds (every person's
account that may log in, or on the lock screen the session's user alone),
and checks the answer's signature with that account's own public key: an
answer that does not verify names nobody. The answer is the owner and its
key's choice (`user=NAME key-pin=0|1 key-touch=0|1 card=0|1`), or `none`,
`many-owners`, `no-key` or `many-keys`. The screen then selects the owner
and asks for the key's PIN (a keypad under the field) and its touch, or for
the touch alone; a lock screen whose account asks neither says "Checking
your security key..." for half a second at least and unlocks. `KEYOWNER` is
not an attempt: it neither counts, clears the counts, delays nor offers the
PIN, and sessiond answers one a second (the others `ERROR busy`; the screen
asks for the last key of a burst). It tells whoever plugs in a registered key
whose account it is, which the key's holder knows anyway. With the lock
screen's card closed a key that comes or goes does nothing, and a sleep
closes the card and cancels the key's attempt under way, so a key left
plugged in never unlocks the screen without the swipe.

**The key's own operations.** Settings' Security Keys page also asks what
key is there, sets or changes the key's PIN, and resets the key, through
sessiond's `KEYINFO`, `KEYPIN set|change` and `KEYRESET` (a session's only;
the greeter cannot ask them) and passkey's `key-info`, `key-set-pin`,
`key-change-pin` and `key-reset`, which passkey-fido2 carries out as
above. What the key is (how many keys, a key's name, whether it has a PIN,
its retries) is told without any secret. Its PIN is checked by the key
alone, which counts the wrong ones itself (eight in all, three per power
cycle), so `KEYINFO` and `KEYPIN` are not attempts of the account: they
neither count, clear the counts, delay nor offer the account's PIN. A reset
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
   `zedbsd.login`, that its flags say the user was present and verified (as
   the account's choice asks: an unlock without the touch does not require
   the user present, a login without the PIN not verified), and that it
   carries no attested data and nothing after its extensions;
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
  in place. Under the lock, the whole file is read, the change is applied,
  the result is written to a new file in `/etc` with the original's mode,
  synced, and renamed over the original. Then `/etc` is synced.
- **The lock:** an exclusive `flock` on `/etc/shadow.lock`. `passwd`, `su`,
  `sudo`, `account-admin`, `passkey` and `passkey-fido2` take it before they
  change an account file or `/etc/passkey`. They try for 5 seconds and then
  answer that the files are busy. The kernel releases the lock when its
  holder exits or dies, so a crashed tool never leaves it held, and a live
  holder's lock is never taken over. The lock file is never removed.
  Removing it would let a waiter lock a file that nobody else sees.
- **wheel:** a user is in `wheel` when it is the primary group or the group
  names the user.
- **Environment:** a command run as another user keeps only the terminal and
  locale variables of the caller and sets `HOME`, `SHELL`, `USER`, `LOGNAME`
  and a fixed `PATH`. `LD_*`, `IFS`, `ENV` and the rest never pass through.

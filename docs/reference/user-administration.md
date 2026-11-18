# useradd, usermod and userdel

Status: reference

`useradd`, `usermod` and `userdel` add, change and remove the people's
accounts from a terminal. They are in `/sbin` and only root runs them,
directly or through `sudo`. They are not set-user-ID.

Each command reads no account file and writes none. It turns its options
into one request and passes it to `/usr/libexec/account-admin`, the program
that also carries out Settings > Users. So the terminal and Settings follow
the same rules: the same names, the same groups, the same protection of
root, the system accounts and the last administrator. The request format,
the checks and what each change does are in
[Account administration](../architecture/security.md#account-administration).

The options follow the Linux shadow-utils commands of the same names, but
only a part of them is offered. POSIX defines no utility for this.

## useradd

```text
useradd [-c COMMENT] [-g GROUP] [-G GROUP[,GROUP]] [-m | -M] [-s SHELL] [-u UID] NAME
```

Adds the user NAME.

| Option | Meaning |
| --- | --- |
| `-c COMMENT` | The display name (the GECOS field): printable, no `:`, at most 64 bytes. Empty by default. |
| `-g GROUP` | The primary group: `wheel`, `network` or NAME (the user's own group, the default). |
| `-G GROUP[,GROUP]` | The other groups: `wheel` and `network`, separated by `,`. |
| `-m` | Makes the home `/home/NAME` with the files of `/etc/skel` (the default). |
| `-M` | Makes no home. |
| `-s SHELL` | The shell: an absolute path to a program that exists. `/bin/sh` by default. |
| `-u UID` | The user ID: from 1000 to 59999, not used by any user or group. By default, one more than the highest ID ever given. |

- The user always gets a private group of the same name and number, even
  with `-g`, so that `usermod -g NAME` can move them back to it.
- The home is always `/home/NAME`. If it is already there, for example kept
  by an earlier `userdel`, the user is not added (status 12): remove the old
  home first, or add the user with `-M`.
- The password is locked (`!` in `/etc/shadow`). Set it with `passwd NAME`.
  Until then the user cannot log in and does not count as an administrator.
- A user in `wheel` is an administrator.

## usermod

```text
usermod [-c COMMENT] [-g GROUP] [-G GROUP[,GROUP] [-a]] [-s SHELL] [-L | -U] NAME
```

Changes the user NAME. At least one option is required.

| Option | Meaning |
| --- | --- |
| `-c COMMENT` | Sets the display name. |
| `-g GROUP` | Sets the primary group: `wheel`, `network` or NAME. |
| `-G GROUP[,GROUP]` | Replaces the user's groups with these (`wheel`, `network`). An empty list, `-G ''`, takes the user out of every group. |
| `-a` | With `-G`: adds the user to the groups instead of replacing them. |
| `-s SHELL` | Sets the shell. |
| `-L` | Locks the password: puts `!` in front of the hash. |
| `-U` | Unlocks the password: takes the `!` off. Refused when no password is behind the lock (a user made by `useradd` with no password set yet); use `passwd NAME`. |

- Not offered: changing the name (`-l`), the user ID (`-u`) or the home
  (`-d`, `-m`), and expiry (`-e`, `-f`).
- `passwd NAME` replaces the whole hash, so it also unlocks a locked
  password.

## userdel

```text
userdel [-r | -f] NAME
```

Removes the user NAME: the lines in `/etc/passwd`, `/etc/shadow` and
`/etc/passkey`, the name in every group, the user's own group (unless
another user uses it as a primary group or it has other members), the
user's cron table and at jobs, and `/etc/keiland/autologin` when it names
the user.

| Option | Meaning |
| --- | --- |
| `-r` | Also removes the home `/home/NAME`, when the user owns it. Nothing on another file system mounted inside it is removed. |
| `-f` | Removes the user even while the user has a running process. |

`-r` and `-f` are not taken together: a running program could keep writing
in the home while it is removed.

## Protection

None of the three commands changes root or a system account (user ID below
1000), and none makes a change that leaves no administrator who can log in
when there was one. This holds for root as well: in a release build root's
password is locked, so the last administrator is the only way to manage the
machine. An administrator who can log in is in `wheel`, has a shell that
logs in, a password that is set and not locked, and an account that has not
expired.

A user ID is never given twice. A removed user's ID is not reused.

Every change goes to the system log (auth) with the user who ran `sudo`.

## Exit status

The statuses follow shadow-utils where it has one for the same case.

| Status | Meaning |
| --- | --- |
| 0 | Done. |
| 1 | Refused or failed: not run by root, root or a system account, the last administrator, no password to unlock, the files could not be changed. |
| 2 | Usage: an unknown option, a missing or extra name, options that are not taken together. |
| 3 | A value outside its rules: the name, the display name, the shell, the user ID, the groups. |
| 4 | The user ID is used by a user or a group. |
| 6 | The user does not exist. |
| 8 | The user has a running process (`userdel` without `-f`). |
| 9 | The name is used by a user or a group. |
| 12 | The home: it is already there, or the user was added but the home could not be made. |

A message on standard error says what happened.

## Examples

```sh
# An administrator, then their password.
sudo useradd -G wheel,network -c 'Tom T' tom
sudo passwd tom

# Lock and unlock an account.
sudo usermod -L tom
sudo usermod -U tom

# Remove the account and its home.
sudo userdel -r tom
```

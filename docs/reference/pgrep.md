# pgrep and pkill

Status: reference

`pgrep` writes the process IDs of the processes it selects. `pkill` sends
them a signal. Both are in `/bin` and select processes the same way: by a
pattern matched against a process's name or command line, and by its user,
group, parent, process group or session.

The options follow FreeBSD's `pgrep` and `pkill`, with the differences listed
at the end. POSIX defines neither utility.

## Synopsis

```text
pgrep [-acfilnoqvx] [-d DELIM] [-g PGRP] [-G GID] [-P PPID] [-s SID] [-u EUID] [-U UID] [PATTERN ...]
pkill [-SIGNAL] [-acfilnovx] [-g PGRP] [-G GID] [-P PPID] [-s SID] [-u EUID] [-U UID] [PATTERN ...]
```

At least one pattern or one list option is required. The options come before
the patterns; `--` ends the options, so a pattern may begin with `-`.

## Selection

A process is selected when it meets every criterion given:

- one of the patterns matches it (each pattern is an extended regular
  expression, as in `grep -E`), and
- its value is in the list of each list option given.

A pattern is matched against the process's **name**, or with `-f` against its
**command line**.

- The name is the last part of the path the process was started by
  (`argv[0]`), without the `-` that a login shell's name begins with:
  `/bin/wayland` is `wayland`, `-sh` is `sh`. A process that set its title
  (`setproctitle`) is named by the program name at the front of the title:
  `sshd: alice [priv]` is `sshd`.
- The command line is `argv[0]` and each argument after it, separated by one
  space, as `ps -o args` shows it. The system keeps its first 255 bytes.

The system shows the name and the command line of another user's process
only to root. For anyone else such a process matches no pattern, but a list
option alone can still select it.

Some processes are never selected:

- the `pgrep` or `pkill` process itself;
- its ancestors (its parent, the parent's parent and so on), unless `-a` is
  given, so that `sh -c 'pkill -f server'` does not signal the shell that
  runs it;
- a process that has already ended (a zombie waiting to be collected by its
  parent), since it can take no signal;
- the kernel's own records.

## Options

| Option | Meaning |
| --- | --- |
| `-SIGNAL` | `pkill` only, and only as the first argument: the signal to send instead of `TERM`. A name (`-KILL`, `-HUP`, `-SIGUSR1`, in any case, with or without `SIG`) or a number (`-9`). `-0` sends no signal and only checks that the process can be signalled. |
| `-a` | Selects the ancestors of `pgrep` or `pkill` too. |
| `-c` | Writes the number of the selected processes instead of their IDs (`pkill`: of the signalled ones). |
| `-d DELIM` | `pgrep` only: writes DELIM between the process IDs instead of a newline. |
| `-f` | Matches the patterns against the command line instead of the name. |
| `-g PGRP` | Selects the processes in one of these process groups. `0` is the caller's own. |
| `-G GID` | Selects the processes of one of these groups (names or numbers). |
| `-i` | Matches the patterns without regard to case. |
| `-l` | `pgrep`: writes the name after each process ID, or with `-f` the command line. `pkill`: writes the `kill` command for each process it signals, for example `kill -15 412`. |
| `-n` | Selects only the newest of the processes that would be selected. |
| `-o` | Selects only the oldest of the processes that would be selected. |
| `-P PPID` | Selects the processes whose parent is one of these. |
| `-q` | `pgrep` only: writes nothing; only the exit status tells. |
| `-s SID` | Selects the processes in one of these sessions. `0` is the caller's own. |
| `-u EUID` | Selects the processes whose effective user is one of these (names or numbers). |
| `-U UID` | Selects the processes whose user is one of these (names or numbers). |
| `-v` | Turns the selection round: selects the processes that do not meet the criteria. |
| `-x` | A pattern must match the whole name, or with `-f` the whole command line. |

A list (PGRP, GID, PPID, SID, EUID, UID) is one or more values separated by
commas. An option given twice adds to its list.

`-n` and `-o` cannot be given together. They apply after `-v`.

## Output and exit status

`pgrep` writes one process ID to a line, in the order of the process IDs.

| Status | Meaning |
| --- | --- |
| 0 | At least one process was selected (`pkill`: signalled). |
| 1 | No process was selected. |
| 2 | A wrong argument: an unknown option or signal, a pattern that is not a regular expression, an unknown user or group, or no criterion at all. |
| 3 | The processes could not be read, or memory ran out. |

`pkill` passes over a process that ended between being selected and being
signalled. A process the caller may not signal (another user's, for anyone
but root) is reported on the standard error and still counts as selected.

## Examples

```sh
# Is a compositor running?
pgrep -x wayland

# Stop it, and wait until it is gone.
pkill -x wayland
while pgrep -qx wayland; do sleep 1; done

# The processes started from a path, with their command lines.
pgrep -lf /home/alice/bin/files

# Kill the newest shell of the user alice.
pkill -KILL -n -u alice -x sh
```

## Differences from FreeBSD

- The name is taken from `argv[0]` or the title, as described above; FreeBSD
  uses the name of the executable file. The system keeps no other name.
- `-U` and `-G` compare the effective user and group: the system does not
  report the real ones (as `ps -U` and `ps -G`).
- `-n` and `-o` choose by the process ID: the system does not report when a
  process started. Process IDs only grow until they wrap round after
  2147483647, so the highest is the newest until then.
- `-n` and `-o` apply after `-v`, and `-v` never selects the caller or, without
  `-a`, its ancestors.
- `-c` is the count, as in Linux's `pgrep`; there are no login classes.
- A process that has ended is never selected.
- `-F`, `-I`, `-L`, `-S`, `-t`, `-j`, `-M` and `-N` are not offered.
- Linux's `pgrep -a` (the command line with each ID) is `pgrep -lf` here; `-a`
  has FreeBSD's meaning.

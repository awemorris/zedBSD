# What remacs needs from the Noct VM, and does not have

This is the platform side of the work list in `docs/design.md` §9.2. That
section sorts the Emacs primitives remacs is missing into *who should
implement them*; this one records the cases where the answer is "nobody
can yet, because the VM cannot reach the operating system that way".

The rule that produced it: a core primitive is implementable when the
Noct VM already exposes the OS access it needs. Everything below is a
primitive whose implementation would have to call something that does
not exist.

**Scale.** Of the 776 core primitives the corpus scan found missing,
**710 are implementable today** and 66 are blocked here. The blocking is
concentrated — five subsystems account for all of it — so the shape of
the next phase is: implement the 710, and fix the VM once per group
below rather than once per function.

## The VM's surface today

Surveyed from the registration tables in `NoctLang/src/api/api-*.c` and
`src/core/intrinsics.c`, not from documentation:

| namespace | what it covers |
|---|---|
| `FileUtil` | `readText`, `readTextEucJp`, `tryReadText`, `writeText`, `tryWriteText`, `readForEachLine`, `writeForEachLine`, `checkFileExists`, `getFileSize`, `listDirectory`, `getCurrentDirectory`, `setCurrentDirectory`, `getHomeDirectory` |
| `Process` | `spawn`, `read`, `write`, `wait`, `isAlive`, `kill`, `close` |
| `System` | `getEnv`, `shell`, `runCommand`, `getOSName`, `import`, `registerSource`, `pcall`, `checkFileExists` |
| `Term` | raw mode, alternate screen, cursor, styles, size, `readKey`, `pendingInput`, synchronized output |
| `Regex` | `search`, `matches`, `replaceAll` |
| `Thread` | thread creation and joins |
| intrinsics | `String`, `Array`, `Dict`, `Packed`, `Math`, `Global`, `GC` |

That is enough for every subsystem that is *editor state* rather than
*operating system*: buffer text, point and markers, windows, keymaps,
text properties, syntax tables, the reader and the printer. It is also
enough for search — `editor/commands.noct` already translates Emacs
regexp syntax onto `Regex.*` and keeps Emacs-style match data.

## The gaps

Each heading is the tag `tools/missing.py` puts in the `platform`
column, so the TSV and this file line up.

### `time` — 21 primitives, and there is no clock at all

**Nothing in the VM tells the time of day.** No `Time` namespace, no
`System.time`, nothing in the intrinsics. This is the largest single
gap and the easiest to close.

Blocked: all of `timefns.c` (`current-time`, `float-time`,
`format-time-string` — 114 files want that one — `decode-time`,
`encode-time`, `time-add`, `time-less-p`, …), plus the file-timestamp
functions that need to compare against now (`file-newer-than-file-p`,
`set-file-times`, `visited-file-modtime`, `verify-visited-file-modtime`,
`set-visited-file-modtime`).

What would close it: a `Time` API returning the current time with
sub-second resolution, plus a UTC/local field breakdown (Emacs's
`decode-time`) and the inverse. `format-time-string` itself is Lisp-side
formatting once the fields are available.

Note that `visited-file-modtime` is also what makes "the file changed on
disk" detection possible, so this gap reaches further into ordinary
editing than the function names suggest.

### `file-stat` — 17 primitives, no `stat(2)`

`FileUtil` answers *does this exist* (by `fopen`) and *how big is it*.
It does not answer *what is it*. `listDirectory` calls `stat` internally
to sort directories from files, but never surfaces the result.

Blocked: `file-attributes` (92 files), `file-directory-p` (161),
`file-readable-p` (119), `file-writable-p` (43), `file-regular-p`,
`file-symlink-p`, `file-executable-p`, `file-modes`,
`file-accessible-directory-p`, `access-file`,
`directory-files-and-attributes`, and the SELinux/ACL pair (which are
non-goals in their own right).

What would close it: one `FileUtil.stat(path)` returning a dictionary —
type (file/dir/symlink), size, mode bits, mtime, uid/gid. Every function
above is derivable from that. `file-readable-p` and friends want
`access(2)` semantics rather than mode arithmetic, so an `access`-style
call is worth having beside it.

This gap and `time` overlap: `file-attributes` needs both.

### `file-manage` — 7 primitives, no rename/copy/link/chmod

Blocked: `rename-file` (41 files), `copy-file`, `add-name-to-file`,
`make-symbolic-link`, `set-file-modes`, `make-temp-file-internal`,
`unix-sync`.

What would close it: `rename`, `unlink`, `mkdir`, `link`, `symlink`,
`chmod` on `FileUtil`. Copying can be Lisp-side once rename and the stat
data exist, but a native copy preserving mode is better.

`make-temp-file-internal` additionally needs a way to create a file
exclusively (`O_EXCL`), which is a security property rather than a
convenience — a Lisp-side "pick a name and hope" version should not be
written.

### `proc-signal` — 5 primitives, only SIGKILL-ish termination

`Process.kill` terminates. It does not take a signal number, so nothing
can send SIGINT, SIGTSTP or SIGCONT to a child.

Blocked: `signal-process`, `interrupt-process`, `stop-process`,
`continue-process`, `quit-process`.

This one already bites: `editor/shell.noct` documents `C-c C-c` as
sending SIGINT, and the gud module's whole interaction model with a
debugger assumes job control.

What would close it: a signal number argument on `Process.kill`.

### `proc-misc` — 6 primitives, the handle is opaque

Blocked: `process-id`, `process-tty-name`, `set-process-window-size`,
`list-system-processes`, `process-attributes`, `num-processors`.

`process-id` wants the pid out of the handle — small. The rest are
system inspection and matter much less; `set-process-window-size` needs
a pty, which `Process.spawn` does not create (it pipes).

### `net` — 5 primitives, no sockets

Blocked: `make-network-process`, `format-network-address`,
`network-interface-list`, `network-lookup-address-info`,
`set-network-process-option`.

The VM has an HTTP server API but no general socket API. Whether remacs
wants network processes at all is a scope question, not just a platform
one — but if the answer is yes, this is what it needs.

### `file-lock` — 3 primitives

`file-locked-p`, `lock-buffer`, `unlock-buffer`. Emacs's `.#file`
symlink protocol needs `symlink` and `readlink`; the `symlink` half is
already listed under `file-manage`.

### `os-users` — 2 primitives

`system-users`, `system-groups`. Passwd/group enumeration. Low value for
a terminal editor; listed for completeness.

## What this implies for sequencing

The 710 ready-now primitives are not waiting on any of this, and they
include everything at the top of the frequency list (`string-match`,
`looking-at`, `erase-buffer`, `narrow-to-region`,
`buffer-substring-no-properties`, `get-buffer`, `skip-chars-forward`).
Nothing here should hold up starting.

When VM work is scheduled, the order that buys the most is:

1. **`time`** — 21 primitives, no design questions, and it unblocks
   `format-time-string`, which 114 files want.
2. **`file-stat`** — 17 primitives from essentially one call.
3. **`proc-signal`** — 5 primitives from one extra argument, and it
   makes two existing remacs modules honest.
4. `file-manage`, then the rest as scope decisions land.

VM changes for this project are made in `~/noct-remacs` only.

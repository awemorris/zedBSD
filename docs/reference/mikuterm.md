# mikuterm

Status: current reference for mikuterm (`apps/terminal-win32`)

mikuterm (Micro Unicode Terminal) is zedBSD's terminal
([`userland/desktop/terminal`](../../userland/desktop/terminal)) built as a
portable Windows program whose tabs are SSH connections. The terminal's core,
libkeiland's Windows layer, libtruetype, libcrypto-compat and zedBSD's own SSH
client ([`userland/base/ssh`](../../userland/base/ssh)) are linked into one
`mikuterm.exe`; it reads and writes only the folder it is in. This document is
the contract of that folder, its settings and what mikuterm asks the user.
How to connect to WSL is a [how-to](../howto/mikuterm-wsl.md).

## 1. Platform

| | |
| --- | --- |
| Windows | Windows 10 version 1903 or later (the manifest's UTF-8 code page needs it), x64 and ARM64 |
| Drawing | On the processor, shown through GDI; no GPU driver is needed |
| Installation | None: the ZIP is unpacked anywhere the user can write. No registry entry, no `%APPDATA%`, no administrator rights |
| Build | CMake in `apps/terminal-win32` with llvm-mingw (`cmake/llvm-mingw-x86_64.cmake`, `-aarch64.cmake`); the `zip` target makes `mikuterm-<version>-win64.zip` and `-winarm64.zip` |

## 2. The folder

The folder is the directory `mikuterm.exe` is in (found from the program's
own path, never from the current directory).

| Path | Written by | Contents |
| --- | --- | --- |
| `mikuterm.exe` | the ZIP | the program |
| `README.txt` | the ZIP | the user's guide |
| `config\mikuterm.conf` | the user | the settings (section 3); the ZIP has a commented sample |
| `config\state.conf` | mikuterm | what the View menu chose: `FontSize`, `Theme`, `AmbiguousWide` |
| `ssh\config` | the user | the hosts, in `ssh_config(5)` form (section 4) |
| `ssh\id_ed25519` | the user | the private key (section 4.3) |
| `ssh\known_hosts` | mikuterm | the hosts' keys the user accepted (section 6) |
| `fonts\` | the user | the user's fonts, found before `share\fonts\` |
| `share\fonts\` | the ZIP | `keiland.ttf`, `keiland-bold.ttf`, `keiland-mono.ttf` (Mahora), `keiland-fallback-mono.ttf` (JetBrains Mono), `keiland-fallback.ttf` (Droid Sans Fallback), `keiland-emoji.ttf` (Noto Color Emoji) |
| `share\licenses\` | the ZIP | the licenses of the program and of each font |

A folder mikuterm cannot write in still runs: the View menu's choices are not
kept and a host's accepted key is accepted for that connection only (the tab
says so).

## 3. `config\mikuterm.conf`

One `Keyword value` (or `Keyword=value`) a line, keywords in any case, `#`
starting a comment, a value in double quotes may hold spaces; the first line
of a keyword counts. A line that cannot be used is named in the first tab
(`line N: why`) and skipped; the rest of the file still applies.

| Keyword | Value | Default |
| --- | --- | --- |
| `Font` | a font file in `fonts\` or `share\fonts\`, or a full path | `keiland-mono.ttf` |
| `FontFallback` | the font for the characters `Font` has not (one file) | `keiland-fallback.ttf` |
| `FontSize` | pixels at 96 dpi, 8 to 32 | 16 |
| `Columns`, `Rows` | the grid the window opens with (1 to 240, 1 to 100) | 90, 28 |
| `Theme` | `dark`, `light`, `contrast` | `dark` |
| `AmbiguousWide` | `yes`, `no`: East Asian Ambiguous characters take two cells | `no` |
| `Term` | the `TERM` sent to the host | `xterm-256color` |
| `Connect` | the host the first tab connects to (a `Host` of `ssh\config`, or `[user@]host[:port]`) | none: the first tab lists the hosts |
| `CopyOnSelect` | `yes`, `no`: the text the pointer selects is also put on the clipboard | `yes` |
| `ConnectTimeout` | seconds for the TCP connection, 0 for no limit (`ssh\config`'s wins) | 15 |

`config\state.conf` has the same form and only `FontSize`, `Theme` and
`AmbiguousWide`; its values win over `mikuterm.conf`'s. Deleting it returns
to `mikuterm.conf`.

## 4. `ssh\config`

### 4.1 Keywords

The file is read with the SSH client's own parser, so its rules are those of
zedBSD's `ssh`:

- Acted on: `Host`, `HostName`, `User`, `Port`, `IdentityFile`,
  `StrictHostKeyChecking` (`yes`, `no`, `ask`, `accept-new`, `off`),
  `UserKnownHostsFile` (the first path), `GlobalKnownHostsFile`, `BatchMode`,
  `ConnectTimeout`, `ServerAliveInterval`, `ServerAliveCountMax`,
  `RequestTTY`, `PasswordAuthentication`, `PubkeyAuthentication`, `LogLevel`.
- Accepted only with a value that changes nothing (for example
  `HashKnownHosts no`, `ForwardAgent no`).
- Anything else, `Match`, `Include` and `ProxyJump` among them, is an error:
  the tab shows `ssh\config: line N: ...` and does not connect. A
  `~/.ssh/config` copied from Linux may need such lines taken out.

`Host` patterns (`*`, `?`, `!`) select the lines that apply. The names
without a pattern character are the list a new tab shows.

### 4.2 Paths

| Written | Means |
| --- | --- |
| `~/.ssh/X` | `<folder>\ssh\X` |
| `~/X` | `<folder>\X` |
| `X`, `dir/X` | `<folder>\ssh\X`, `<folder>\ssh\dir\X` |
| `C:\...`, `\\server\...` | as written |
| `/...` | no file on Windows (`/dev/null` keeps nothing, section 6) |

The defaults are OpenSSH's: `IdentityFile ~/.ssh/id_ed25519`,
`UserKnownHostsFile ~/.ssh/known_hosts`, and `GlobalKnownHostsFile
/etc/ssh/ssh_known_hosts`, which names no file here.

### 4.3 Keys and algorithms

- Keys: OpenSSH's private key format with an `ssh-ed25519` key and no
  passphrase. A key with a passphrase is passed over.
- Key exchange: `mlkem768x25519-sha256`, then `curve25519-sha256`
  (OpenSSH 8.x and 9.x servers, WSL's Ubuntu among them). Host key:
  `ssh-ed25519`. Cipher: `chacha20-poly1305@openssh.com`. Strict key
  exchange is required.
- Authentication: the keys, then the password. keyboard-interactive and
  agents are not supported.

## 5. Tabs and questions

A new tab (Shell > New Tab, Ctrl+Shift+T) lists the names of `ssh\config`
with numbers and asks `Host: `; the answer is a name, a number, or
`[user@]host[:port]` (an IPv6 address in brackets). The first tab connects to
`Connect` without asking when it is set.

The questions are answered on a line of the tab: Backspace and Ctrl+U edit
it, Enter answers, Ctrl+C or Ctrl+D gives the question up. They follow
OpenSSH's `ssh` where it has the same question:

| When | The tab shows | Answer |
| --- | --- | --- |
| No `User` for the host | `login as: ` | a name; an empty one is asked again |
| A host's key is not known (`StrictHostKeyChecking ask`) | `The authenticity of host 'NAME (ADDRESS)' can't be established.`, `ED25519 key fingerprint is SHA256:...`, the other names the key is known by in known_hosts (or `This key is not known by any other names.`), `Are you sure you want to continue connecting (yes/no/[fingerprint])? ` | `yes` (any case) or the fingerprint: trust it and keep it; `no` or nothing: refuse; anything else: `Please type 'yes', 'no' or the fingerprint: ` |
| The server asks for the password | `USER@HOST's password: `, then `Permission denied, please try again.` after a refused one | the password, not shown |
| The host showed another key than the one known | the `WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!` frame, the fingerprint of the key shown, `Offending ED25519 key in FILE:LINE`, `Host key for NAME has changed and you have requested strict checking.` | none: the connection is refused |
| The key is on an `@revoked` line | the `WARNING: REVOKED HOST KEY DETECTED!` frame and the file and line | none: refused |
| An unknown key with `StrictHostKeyChecking yes` or `BatchMode yes` | `No ED25519 host key is known for NAME and you have requested strict checking.` | none: refused |

`NAME` is the host as known_hosts names it (`host`, or `[host]:port` for a
port other than 22, in lower case); `ADDRESS` is the address reached, in the
same form. A file inside the folder is shown by its path in the folder
(`ssh\known_hosts`).

While a question asks for a secret (the password), the tab's input does not
echo and the window's input method is off. A password a program on the host
asks for (sudo, su) cannot be told from other input: Session > Disable Input
Method (Ctrl+Shift+I) turns the input method off until it is chosen again.
F10 is the shortcut of View > Treat Ambiguous-Width Characters as Wide.

A connection that ends normally closes its tab. One that fails shows why
(`Host key verification failed.`, `USER@HOST: Permission denied (METHODS).`,
`ssh: connect to host HOST port PORT: ...`) and
`Press Enter to connect again (Ctrl+C closes the tab).`

## 6. known_hosts

- Read: the user's file and the global one, in OpenSSH's format
  (`sshd(8)` SSH_KNOWN_HOSTS FILE FORMAT). Lines whose names are hashed
  (`|1|...`) cannot be matched and are passed over, so a host known only by a
  hashed line is asked about again. `@cert-authority` lines are passed over;
  `@revoked` lines are honoured.
- Written: when the user accepts a key (or `StrictHostKeyChecking` is
  `accept-new` or `no`), one line is appended to the user's file:
  `NAME ssh-ed25519 KEY`, the name in lower case, never hashed
  (`HashKnownHosts yes` is an error in `ssh\config`). The line is written in
  one write, so two mikuterm windows appending at once keep both lines whole.
  A file whose last line has no line feed gets one before the new line. A
  missing file is created, and a missing directory one level above it too.
  The tab then shows `Warning: Permanently added 'NAME' (ED25519) to the list
  of known hosts.`
- A file that cannot be written: the tab shows `Failed to add the host to the
  list of known hosts (FILE).` and the key is trusted for that connection
  only.
- `UserKnownHostsFile /dev/null`: nothing is read or kept.
- A changed key is never replaced from the tab: the user deletes the line the
  warning names.

## 7. Menus

mikuterm has the menus of zedBSD's terminal, with these differences:

| Item | Shortcut | |
| --- | --- | --- |
| Session > Disable Input Method | Ctrl+Shift+I | mikuterm only; checked while the user keeps the input method off |
| View > Treat Ambiguous-Width Characters as Wide | F10 | F10 in mikuterm only (zedBSD's compositor opens the menus with F10); F10 then does not reach the host |
| Shell > New Window | Ctrl+Shift+N | starts another `mikuterm.exe` |

## 8. Licenses

`mikuterm.exe` is Awe Morris's work under the zlib license. The fonts in
`share\fonts\` are their authors' works under their own licenses (SIL Open
Font License 1.1, Apache License 2.0, Mahora's), each in `share\licenses\`.
The terminal's East Asian width table derives from the Unicode Character
Database (`Unicode-License-V3.txt`).

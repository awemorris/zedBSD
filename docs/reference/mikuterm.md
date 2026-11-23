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
| `config/mikuterm.conf` | the user, and the settings tab in place of a keyword's line | the settings (section 3); the ZIP has a commented sample |
| `config/state.conf` | mikuterm | what the View menu and the settings tab chose at once: `FontSize`, `Theme`, `AmbiguousWide`, `CopyOnSelect` |
| `ssh/config` | the user (also from the settings tab) | the hosts, in `ssh_config(5)` form (section 4) |
| `ssh/id_ed25519` | the user, or the settings tab's Make Key | the private key (section 4.3) |
| `ssh/known_hosts` | mikuterm | the hosts' keys the user accepted (section 6) |
| `fonts/` | the user | the user's fonts, found before `share/fonts/` |
| `share/fonts/` | the ZIP | `keiland.ttf`, `keiland-bold.ttf`, `keiland-mono.ttf` (Mahora), `keiland-fallback-mono.ttf` (JetBrains Mono), `keiland-fallback.ttf` (Droid Sans Fallback), `keiland-emoji.ttf` (Noto Color Emoji) |
| `share/licenses/` | the ZIP | the licenses of the program and of each font |

mikuterm names a path with `/` between its folders wherever it shows one (the
settings tab, a tab's messages and questions, the problems of
`config/mikuterm.conf`): a file of the folder by its path inside it
(`ssh/known_hosts`), any other by its whole path (`C:/Users/me/known_hosts`).
Windows takes `/` and `\` alike, so a path shown opens the same file when it is
typed or pasted; this document writes paths the same way. The paths mikuterm
opens are its own and are not changed by how they are shown.

A folder mikuterm cannot write in still runs: the View menu's choices are not
kept and a host's accepted key is accepted for that connection only (the tab
says so).

## 3. `config/mikuterm.conf`

One `Keyword value` (or `Keyword=value`) a line, keywords in any case, `#`
starting a comment, a value in double quotes may hold spaces; the first line
of a keyword counts. A line that cannot be used is named in the first tab
(`line N: why`) and skipped; the rest of the file still applies.

| Keyword | Value | Default |
| --- | --- | --- |
| `Font` | a font file in `fonts/` or `share/fonts/`, or a full path | `keiland-mono.ttf` |
| `FontFallback` | the font for the characters `Font` has not (one file) | `keiland-fallback.ttf` |
| `FontSize` | pixels at 96 dpi, 8 to 32 | 16 |
| `Columns`, `Rows` | the grid the window opens with (1 to 240, 1 to 100) | 90, 28 |
| `Theme` | `dark`, `light`, `contrast` | `dark` |
| `AmbiguousWide` | `yes`, `no`: East Asian Ambiguous characters take two cells | `no` |
| `Term` | the `TERM` sent to the host | `xterm-256color` |
| `Connect` | the host the first tab connects to (a `Host` of `ssh/config`, or `[user@]host[:port]`) | none: the first tab lists the hosts |
| `CopyOnSelect` | `yes`, `no`: the text the pointer selects is also put on the clipboard | `yes` |
| `ConnectTimeout` | seconds for the TCP connection, 0 for no limit (`ssh/config`'s wins) | 15 |
| `WslPort` | the port on `localhost` of the sshd in WSL, for a distribution without a `Host wsl-NAME` in `ssh/config` (1 to 65535) | 2222 |

A full path for `Font` and `FontFallback` starts at a drive (`C:/...`) or a
server (`//server/share/...`), with `/` or `\`. Anything else, a path from the
root of the current drive (`/Windows/...`, `\Windows\...`) or a drive without
a separator after it (`C:x.ttf`) among them, is a name in `fonts/` or
`share/fonts/`.

`config/state.conf` has the same form and only `FontSize`, `Theme`,
`AmbiguousWide` and `CopyOnSelect`; its values win over `mikuterm.conf`'s.
Deleting it returns to `mikuterm.conf`.

The settings tab (section 7) writes the other keywords into
`mikuterm.conf` itself: the first line of the keyword is replaced by
`Keyword value` (quoted when the value has a space), every other line and
comment is kept as it was, a keyword without a line is added at the end, and
an empty value removes the line so the default holds. A value is checked as
the file would be read before it is written.

## 4. `ssh/config`

### 4.1 Keywords

The file is read with the SSH client's own parser, so its rules are those of
zedBSD's `ssh`:

- Acted on: `Host`, `HostName`, `User`, `Port`, `IdentityFile`,
  `StrictHostKeyChecking` (`yes`, `no`, `ask`, `accept-new`, `off`),
  `UserKnownHostsFile` (the first path), `GlobalKnownHostsFile`, `BatchMode`,
  `ConnectTimeout`, `ServerAliveInterval`, `ServerAliveCountMax`,
  `PasswordAuthentication`, `PubkeyAuthentication`,
  `ForwardX11`, `ForwardX11Trusted`, `ForwardX11Timeout`, `LocalForward`,
  `RemoteForward`, `GatewayPorts`, `ExitOnForwardFailure`,
  `ClearAllForwardings`, `RemoteCommand` (section 4.4).
- Read and checked, but changing nothing in mikuterm: `RequestTTY` (a tab is
  a terminal, so mikuterm always asks the server for one, for a
  `RemoteCommand` too) and `LogLevel` (mikuterm shows no debug lines). The
  values are still checked, so a line zedBSD's `ssh` would refuse is an error
  here too.
- `SessionType none` (a connection that only forwards ports) is read but
  refused: the tab says `ssh/config: SessionType none is not supported` and
  does not connect.
- Accepted only with a value that changes nothing (for example
  `HashKnownHosts no`, `ForwardAgent no`).
- Anything else, `Match`, `Include` and `ProxyJump` among them, is an error:
  the tab shows `ssh/config: line N: ...` and does not connect. A
  `~/.ssh/config` copied from Linux may need such lines taken out.

`Host` patterns (`*`, `?`, `!`) select the lines that apply. The names
without a pattern character are the list a new tab shows.

### 4.2 Paths

| Written | Means |
| --- | --- |
| `~/.ssh/X` | `<folder>/ssh/X` |
| `~/X` | `<folder>/X` |
| `X`, `dir/X` | `<folder>/ssh/X`, `<folder>/ssh/dir/X` |
| `C:/...`, `//server/share/...` (or with `\`) | as written |
| any other `/...` (`//name` with no `/` after it too) | no file on Windows (`/dev/null` keeps nothing, section 6) |

The defaults are OpenSSH's: `IdentityFile ~/.ssh/id_ed25519`,
`UserKnownHostsFile ~/.ssh/known_hosts`, and `GlobalKnownHostsFile
/etc/ssh/ssh_known_hosts`, which names no file here.

### 4.3 Keys and algorithms

- Keys: OpenSSH's private key format with an `ssh-ed25519` key, in the
  clear or encrypted as `ssh-keygen` writes it with a passphrase (cipher
  `aes256-ctr`, KDF `bcrypt`, up to 16384 rounds). Another cipher
  (`ssh-keygen -Z`) is not supported, and such a key is passed over.
- Key exchange: `mlkem768x25519-sha256`, then `curve25519-sha256`
  (OpenSSH 8.x and 9.x servers, WSL's Ubuntu among them). Host key:
  `ssh-ed25519`. Cipher: `chacha20-poly1305@openssh.com`. Strict key
  exchange is required.
- Authentication: the keys, then the password. keyboard-interactive and
  agents are not supported.

### 4.4 Port forwarding and `RemoteCommand`

A host's `LocalForward` and `RemoteForward` lines forward TCP ports as
OpenSSH's `ssh -L` and `ssh -R` do, and as zedBSD's `ssh` does:

```
Host db
    HostName server.example.org
    LocalForward 8080 127.0.0.1:80
    RemoteForward 9000 localhost:3000
```

- `LocalForward [address:]port host:hostport`: mikuterm listens on this PC
  and each connection to it goes through the SSH connection to `host` and
  `hostport` as the server reaches them. Without an address it listens on
  the loopback addresses (`127.0.0.1` and `::1`) only; with `GatewayPorts
  yes`, or the address `*` or empty, on every address of the PC (other
  computers can then use the port); `localhost` is the loopback addresses;
  another address (or a name, looked up) is that address only.
- `RemoteForward [address:]port host:hostport`: the server listens on
  `port` (on `localhost` unless an address is given; its sshd's
  `GatewayPorts` decides what it allows) and each connection to it comes to
  `host` and `hostport` as this PC reaches them. A connection that cannot be
  made in 30 seconds is refused. `port` 0 lets the server choose.
- `ExitOnForwardFailure yes` ends the connection when a forwarding cannot be
  set up; `ClearAllForwardings yes` drops every forwarding of the host.
- The forwardings start once the user is logged in and last as long as the
  connection. When the shell ends while forwarded connections are open, the
  connection stays until they end (the tab says so); close the tab to end
  them. Each tab connecting to the host has its own forwardings: a second
  tab with the same `LocalForward` cannot listen on the port.
- A listening socket is exclusive (`SO_EXCLUSIVEADDRUSE`): no other program
  can take the port while mikuterm listens on it. When the connection is
  made again (Enter after a failure), a port that its last connections
  still hold is tried once more after 2 seconds.
- Listening on every address may make Windows Defender Firewall ask whether
  mikuterm may accept connections from the network. On the loopback
  addresses anyone using this PC can connect to the port, as with OpenSSH.

The tab shows OpenSSH's short lines when a forwarding is not set up as
asked, and nothing when it is:

| When | The tab shows |
| --- | --- |
| A `LocalForward` cannot listen on an address | `bind [127.0.0.1]:8080: Address already in use` (the last address tried) |
| No `LocalForward` can listen | `Could not request local forwarding.` |
| The server refuses a `RemoteForward` | `Warning: remote port forwarding failed for listen port 9000` |
| The server chose the port of `RemoteForward 0 ...` | `Allocated port 40123 for remote forward to localhost:3000` |
| `ExitOnForwardFailure yes` and a forwarding failed | the connection ends with `Could not request local forwarding.` or `Error: remote port forwarding failed for listen port 9000`, and Enter connects again |

A forwarded connection that the server or this PC cannot make (OpenSSH's
`channel N: open failed:` and `connect_to ...` lines) is closed without a
line in the tab. Dynamic forwarding (`DynamicForward`, SOCKS), forwarding
Unix sockets, and a `RemoteForward` without a destination (a SOCKS proxy on
the server) are not supported; `DynamicForward` is an error of
`ssh/config`.

`RemoteCommand` runs that command instead of the shell, on a terminal as the
shell would be (OpenSSH's and zedBSD's `ssh` ask for a terminal for a command
only with `-t` or `RequestTTY`; mikuterm always does). Its tokens are
OpenSSH's: `%%`, `%h` (the host's address), `%n` and `%k` (the name as
given), `%p`, `%r` (the remote user), `%u` (the Windows user), `%d` (the
user's profile folder), `%l` (this PC's name), `%L` (that name up to its
first dot) and `%j` (empty). `%C` and `%i` (Windows has no user number) are
errors, as are an unknown token and a `%` at the end; the tab then names the
problem and does not connect.

How a forwarded port differs from OpenSSH's: the server's connections to a
`RemoteForward` are refused once the shell ended (OpenSSH takes them while
the connection lasts).

## 5. Tabs and questions

A new tab (Shell > New Tab, Ctrl+Shift+T) shows the launch screen; the first
tab connects to `Connect` without it when that is set. A window has at most
ten tabs, the settings tab among them.

The keys go between the tabs (the tab bar shows them from two):

| Keys | Tab |
| --- | --- |
| Ctrl+Alt+1 to Ctrl+Alt+9 | the tab of that place, counted from the left |
| Ctrl+Alt+0 | the tenth tab |
| Ctrl+Alt+Page Up | the tab before the active one (the last after the first) |
| Ctrl+Alt+Page Down | the tab after it (the first after the last) |

The digits are those of the top row, whatever the keyboard layout types on
them; a place with no tab does nothing. These keys are mikuterm's and never
reach the host. They are taken with the left Alt key only: the right Alt
key is AltGr on the layouts that have one (Windows reports it as Ctrl+Alt),
and AltGr with a digit types its character to the host as before. Ctrl with
the left Alt types no AltGr character in mikuterm, as these keys take it.

A tab's title is the number of its connection in the order the tabs were
opened and `mikuterm` (`1 mikuterm`, `2 mikuterm`, the settings tab not
counted; the number first, so that a narrow tab still shows it), until the
host sets one. When the tabs do not fit in the title
bar at their least width (96 pixels at 100 %), the tab bar scrolls as the
desktop's titlebar in zedBSD does: an arrow at each end moves it by one tab
(faint at its end), the wheel over the tabs moves it too, and the button
after the new tab's (˅) lists the tabs out of sight. A tab chosen with the
keys or from the list, a new or closed tab, and a change of the window's
size that shows another number of tabs bring the active tab into sight; a
change of a title alone leaves the tab bar where it was.

```
MikμTerm 0.1.0

Hosts:
 [1] me@server
 [2] backup
 [w] WSL
 [x] Settings

>>> me@server
```

- The first line is the program's name and version. A notice of
  `mikuterm.conf`'s problems (the first tab's) comes under it.
- `Hosts:` lists the names of `ssh/config`'s `Host` lines (the ones without
  `*`, `?` or `!`, each once, at most 32), numbered from 1, shown as
  `USER@NAME` when the host's settings give a `User`; then `[w] WSL` and
  `[x] Settings`.
- The chosen row is drawn with the background `#39C5BB` and dark text, in
  every theme. When the rows do not fit the grid, the ones round the chosen
  row are shown.
- The `>>> ` line holds the chosen row's text. Up and Down choose a row;
  while the line holds a row's text, the row's key (its number, a second digit
  making 10 and above, `w`, `x`) chooses that row, and any other character
  starts a line typed by hand in its place. Backspace and Ctrl+U edit the
  line; a paste (bracketed) is always text and takes the row's place. Escape
  puts the chosen row's text back.
- Enter opens the chosen row, or the row whose text the line is; any other
  line is a host typed by hand, `[user@]host[:port]` (an IPv6 address in
  brackets), or a `Host` of `ssh/config` by name. An empty line does
  nothing. A name that starts with a digit, `w` or `x` is typed after
  Ctrl+U.
- `[x] Settings` opens the settings tab beside this one (section 7); this
  tab stays on the screen. Ctrl+C or Ctrl+D closes the tab.
- `[w] WSL` runs `wsl.exe -l -q` (the one in the Windows system folder,
  without a console window, at most 15 seconds) and reads its output
  (UTF-16LE with or without a byte order mark, or UTF-8 when `WSL_UTF8=1`).
  The distributions are listed the same way under `WSL distributions:`, each
  with where it is reached, and `[b] Back` (or Escape) returns to the hosts.
  When there is no `wsl.exe`, no distribution, wsl.exe fails (its first line
  is shown) or does not answer in time, the hosts' screen says so.
- A distribution chosen is started with `wsl.exe -d NAME -e whoami` (at most
  60 seconds; it also starts an sshd systemd runs in it) and reached by SSH:
  through `ssh/config`'s `Host wsl-NAME` (the name in any case) with all that
  Host's settings, or else at `localhost` on `WslPort`. The user is the
  Host's `User`, else the user `whoami` answered, else `login as:` asks. The
  distributions of WSL 2 share one network, so each needs its own port (and
  its own Host) when more than one runs an sshd. A connection to a
  distribution that fails adds `NAME needs an sshd listening on port PORT`.

The questions are answered on a line of the tab: Backspace and Ctrl+U edit
it, Enter answers, Ctrl+C or Ctrl+D gives the question up. They follow
OpenSSH's `ssh` where it has the same question:

| When | The tab shows | Answer |
| --- | --- | --- |
| No `User` for the host | `login as: ` | a name; an empty one is asked again |
| A host's key is not known (`StrictHostKeyChecking ask`) | `The authenticity of host 'NAME (ADDRESS)' can't be established.`, `ED25519 key fingerprint is SHA256:...`, the other names the key is known by in known_hosts (or `This key is not known by any other names.`), `Are you sure you want to continue connecting (yes/no/[fingerprint])? ` | `yes` (any case) or the fingerprint: trust it and keep it; `no` or nothing: refuse; anything else: `Please type 'yes', 'no' or the fingerprint: ` |
| The server would take an encrypted key | `[x] Remember the passphrase until you sign out of Windows (Tab switches this)` and `Enter passphrase for key 'FILE': `; the same again after a wrong one, three times at most | the passphrase, not shown; Tab switches the box; nothing, or Ctrl+C, passes the key over |
| The server asks for the password | `USER@HOST's password: `, then `Permission denied, please try again.` after a refused one | the password, not shown |
| The host showed another key than the one known | the `WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!` frame, the fingerprint of the key shown, `Offending ED25519 key in FILE:LINE`, `Host key for NAME has changed and you have requested strict checking.` | none: the connection is refused |
| The key is on an `@revoked` line | the `WARNING: REVOKED HOST KEY DETECTED!` frame and the file and line | none: refused |
| An unknown key with `StrictHostKeyChecking yes` or `BatchMode yes` | `No ED25519 host key is known for NAME and you have requested strict checking.` | none: refused |

`NAME` is the host as known_hosts names it (`host`, or `[host]:port` for a
port other than 22, in lower case); `ADDRESS` is the address reached, in the
same form. A file inside the folder is shown by its path in the folder
(`ssh/known_hosts`).

While a question asks for a secret (a passphrase, the password), the tab's input does not
echo and the window's input method is off. A password a program on the host
asks for (sudo, su) cannot be told from other input: Session > Disable Input
Method (Ctrl+Shift+I) turns the input method off until it is chosen again.

A passphrase that opened its key while the box was checked is kept in
Windows' Credential Manager as a generic credential named
`mikuterm:passphrase:` and the key file's full path, for the Windows logon
session only (`CRED_PERSIST_SESSION`: held in memory, never written to the
disk, gone when the user signs out). The next connection with that key uses
it without asking; one that no longer opens the key is forgotten and the
question is asked. Session > Forget Remembered Passphrases deletes every
credential of mikuterm's. Nothing else of the user's credentials is read or
changed.

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
  (`HashKnownHosts yes` is an error in `ssh/config`). The line is written in
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

## 7. The settings tab

Shell > Settings... (F12) opens the settings as a tab of their own, or
chooses it when it is open; F12 on it goes back to the tab it was opened
from. `[x] Settings` on a new tab's launch screen (section 5) opens it, or
chooses it, the same way, and the launch screen's tab stays. The tab is a page
of libkeiland's widgets with a sidebar of four panes; Ctrl+Down or Ctrl+Page
Down shows the next pane and Ctrl+Up or Ctrl+Page Up the one before, Tab moves
between a pane's controls. Ctrl+Up and Ctrl+Down are the keys that work in
zedBSD's terminal as well, whose desktop takes Ctrl+Page Up and Ctrl+Page Down
for the titlebar's tabs. The page follows the window's scale (the monitor's
dpi), as the grid does.

Each card's rows put their labels in one column, as wide as the pane's widest
label (within a least and a most), and the row's field, switch or buttons right
after it; a field is at most 420 units (pixels at 100 %) wide, so a wide window
does not stretch it.

| Pane | Setting | Takes effect | Kept in |
| --- | --- | --- | --- |
| Hosts | `ssh/config`, a host at a time (below); Open in Text Editor for the rest | the next connection | `ssh/config` |
| Appearance | text size (Smaller, Larger, Small to Huge), theme, ambiguous width | at once, as the View menu | `config/state.conf` |
| Appearance | `Font`, `FontFallback` (a file in `fonts/` or `share/fonts/`, or a full path; checked to be there), `Columns`, `Rows` | the next start | `config/mikuterm.conf` |
| Behavior | copy-on-select | at once | `config/state.conf` |
| Behavior | `Term`, `ConnectTimeout`, `WslPort` | the next connection | `config/mikuterm.conf` |
| Behavior | `Connect` | the next start | `config/mikuterm.conf` |
| Keys | a new key, a key's public key, `ssh/known_hosts` | at once | `ssh/` |

Hosts pane:

- The list: each `Host` (its names, and `user@address:port` from its own
  lines), the lines before any `Host` when there are some, and each `Match`
  (shown, changed only in a text editor). New host and Add Host put a `Host`
  block where `ssh` reads it before a block that would win over it: before
  the first `Host` whose patterns match the name, or the first `Match`, else
  at the end.
- The form of the host chosen: Name (`Host`), Host address (`HostName`),
  Port, User, Command (`RemoteCommand`, as written), Identity files
  (`IdentityFile`, in order), and Port forwarding (`LocalForward`,
  `RemoteForward`: where it listens, `[address:]port`, and where it goes,
  `host:port`, an IPv6 address in brackets). A value that another block
  earlier in the file sets for the same host says `Overridden by line N`
  (the first value `ssh` obtains is used); identity files and forwards that
  other blocks add are shown with their lines. Other lines lists the block's
  lines the form does not show (comments, other keywords, `Include`, a second
  line of a keyword that `ssh` passes over).
- Only the lines a change is about are rewritten: a value in place (the
  keyword's spelling, `=`, indent and a comment after it kept), a line added
  after the block's lines (indented as they are, ending CR LF or LF as the
  line before it), a line taken out. Remove Host takes out the `Host` line and
  its keywords' lines, and keeps the comments.
- Save (Ctrl+S) checks the text as a connection reads it and writes nothing
  when a line it changed cannot be read (`Not saved: line N: ...`); a problem
  the file had already does not stop it. It does not write over a file
  changed outside since it was read; Revert (Ctrl+R) reads the file again,
  the changes not saved dropped.
- Open in Text Editor opens `ssh/config` (made empty when it is not there) in
  the editor Windows has for it (its `edit`, else its `open`, else Notepad).
  The pane does not read the editor's changes until Revert or the next start;
  connections read the file each time.

Keys pane:

- Name: the key's file name in `ssh/` (letters, digits, `.`, `_`, `-`; not
  starting with `.` or ending with `.pub`; `id_ed25519` by default). When a
  key of the name is there, its public key line and SHA256 fingerprint are
  shown (from `NAME.pub`, or from the private key file's public half), with
  Copy Public Key.
- Otherwise Comment (`user@computer` by default), Passphrase and Again, and
  Make Key: a new `ssh-ed25519` key from the system's random generator
  (`BCryptGenRandom`), written as `ssh/NAME` (OpenSSH's private key format;
  with a passphrase encrypted with `aes256-ctr` under the `bcrypt` KDF, a
  16-byte salt and 24 rounds, as `ssh-keygen` writes it) and `ssh/NAME.pub`
  (the one-line public key). A file of either name that is there is never
  written over. The private key file is made with a protected access list
  that gives only the user who runs mikuterm access to it.
- Known hosts: each line of `ssh/known_hosts` with its names, key type and
  fingerprint, and a button that removes the line (asked once more); the
  file is written whole without it.

zedBSD's terminal has the same tab with its own files and settings in place
of the folder's: `~/.ssh/`, and Font, Fallback font, Columns, Rows and TERM
kept in the desktop's settings ([Terminal](terminal.md), section 3). The
settings that only connections have (`ConnectTimeout`, `WslPort`, `Connect`)
are mikuterm's alone.

## 8. Menus

mikuterm has the menus of zedBSD's terminal, with these differences:

| Item | Shortcut | |
| --- | --- | --- |
| Session > Disable Input Method | Ctrl+Shift+I | mikuterm only; checked while the user keeps the input method off |
| Session > Forget Remembered Passphrases | none | mikuterm only; forgets the passphrases kept in Credential Manager |
| View > Fullscreen | Alt+Enter | in the place of F11 (zedBSD's terminal keeps F11) |
| Shell > Settings... | F12 | the settings tab (section 7), as in zedBSD's terminal |
| Shell > New Window | Ctrl+Shift+N | starts another `mikuterm.exe` |

F1 to F11 alone reach the host as keys (htop and mc use F10 and F11); F12
opens the settings, and Alt+F11 is View > Treat Ambiguous-Width Characters
as Wide, as in zedBSD's terminal. Ctrl+Alt with the top row's digits, Page Up and Page
Down choose the tabs (section 5); zedBSD's terminal leaves the tabs' keys to
its desktop's titlebar (Ctrl+Tab, Ctrl+Page Up and Ctrl+Page Down).

## 9. Licenses

`mikuterm.exe` is Awe Morris's work under the zlib license. The fonts in
`share/fonts/` are their authors' works under their own licenses (SIL Open
Font License 1.1, Apache License 2.0, Mahora's), each in `share/licenses/`.
The terminal's East Asian width table derives from the Unicode Character
Database (`Unicode-License-V3.txt`).

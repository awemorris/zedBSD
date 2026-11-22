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
| `config\mikuterm.conf` | the user, and the settings tab in place of a keyword's line | the settings (section 3); the ZIP has a commented sample |
| `config\state.conf` | mikuterm | what the View menu and the settings tab chose at once: `FontSize`, `Theme`, `AmbiguousWide`, `CopyOnSelect` |
| `ssh\config` | the user (also from the settings tab) | the hosts, in `ssh_config(5)` form (section 4) |
| `ssh\id_ed25519` | the user, or the settings tab's Make Key | the private key (section 4.3) |
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
| `WslPort` | the port on `localhost` of the sshd in WSL, for a distribution without a `Host wsl-NAME` in `ssh\config` (1 to 65535) | 2222 |

`config\state.conf` has the same form and only `FontSize`, `Theme`,
`AmbiguousWide` and `CopyOnSelect`; its values win over `mikuterm.conf`'s.
Deleting it returns to `mikuterm.conf`.

The settings tab (section 7) writes the other keywords into
`mikuterm.conf` itself: the first line of the keyword is replaced by
`Keyword value` (quoted when the value has a space), every other line and
comment is kept as it was, a keyword without a line is added at the end, and
an empty value removes the line so the default holds. A value is checked as
the file would be read before it is written.

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

## 5. Tabs and questions

A new tab (Shell > New Tab, Ctrl+Shift+T) shows the launch screen; the first
tab connects to `Connect` without it when that is set.

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
- `Hosts:` lists the names of `ssh\config`'s `Host` lines (the ones without
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
  brackets), or a `Host` of `ssh\config` by name. An empty line does
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
  through `ssh\config`'s `Host wsl-NAME` (the name in any case) with all that
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
(`ssh\known_hosts`).

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
does not stretch it. The tab names the folder's files with `/` (`ssh/config`
is the folder's `ssh\config`; Windows reads both), as the table below does.

| Pane | Setting | Takes effect | Kept in |
| --- | --- | --- | --- |
| Hosts | `ssh/config`, edited as text (tabs shown as four spaces; CR LF kept); Save writes it whole and names the first line the SSH client's parser refuses (`line N: ...`), the file kept all the same | the next connection | `ssh/config` |
| Appearance | text size (Smaller, Larger, Small to Huge), theme, ambiguous width | at once, as the View menu | `config/state.conf` |
| Appearance | `Font`, `FontFallback` (a file in `fonts/` or `share/fonts/`, or a full path; checked to be there), `Columns`, `Rows` | the next start | `config/mikuterm.conf` |
| Behavior | copy-on-select | at once | `config/state.conf` |
| Behavior | `Term`, `ConnectTimeout`, `WslPort` | the next connection | `config/mikuterm.conf` |
| Behavior | `Connect` | the next start | `config/mikuterm.conf` |
| Keys | a new key, a key's public key, `ssh/known_hosts` | at once | `ssh/` |

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
| View > Treat Ambiguous-Width Characters as Wide | Alt+F11 | mikuterm only |
| View > Fullscreen | Alt+Enter | in the place of F11 (zedBSD's terminal keeps F11) |
| Shell > Settings... | F12 | the settings tab (section 7), as in zedBSD's terminal |
| Shell > New Window | Ctrl+Shift+N | starts another `mikuterm.exe` |

F1 to F11 alone reach the host as keys (htop and mc use F10 and F11); F12
opens the settings.

## 9. Licenses

`mikuterm.exe` is Awe Morris's work under the zlib license. The fonts in
`share\fonts\` are their authors' works under their own licenses (SIL Open
Font License 1.1, Apache License 2.0, Mahora's), each in `share\licenses\`.
The terminal's East Asian width table derives from the Unicode Character
Database (`Unicode-License-V3.txt`).

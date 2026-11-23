# Terminal

Status: current reference for zedBSD's terminal (`userland/desktop/terminal`)

Terminal is the desktop's VT100 terminal: a Wayland window drawn with Vulkan
whose tabs each run a shell on a pseudo-terminal. Its core (the emulator, the
tabs, the menus and the settings tab) is shared with
[mikuterm](mikuterm.md), the Windows build whose tabs are SSH connections.
This document is the contract of the terminal's command line, its kept
settings, its settings tab, its tabs and its menus.

## 1. Command line

```
terminal [--display=NAME] [--font=PATH] [--fallback-font=PATH] [--font-size=PIXELS]
         [--columns=N] [--rows=N] [--command=COMMAND] [--token=NAME] [--timeout-s=N]
```

| Option | Meaning | Without it |
| --- | --- | --- |
| `--display=NAME` | the Wayland display | the session's display |
| `--font=PATH` | the font file the grid is drawn with | `terminal.font`, else `/usr/share/fonts/keiland-mono.ttf` |
| `--fallback-font=PATH` | the font for the characters the font has not (CJK) | `terminal.fallback-font`, else `/usr/share/fonts/keiland-fallback.ttf` |
| `--font-size=PIXELS` | the text's size in pixels at 100 % | `terminal.font-size`, else 16 |
| `--columns=N`, `--rows=N` | the grid the window opens with (1 to 240, 1 to 100) | `terminal.columns`, `terminal.rows`, else 90 and 28 |
| `--command=COMMAND` | each tab runs `/bin/sh -c COMMAND` instead of `/bin/sh` | an interactive shell |
| `--token=NAME` | the name of the run in its log lines | `term` |
| `--timeout-s=N` | ends the run after N seconds | no limit |

An option given on the command line wins over the kept setting, and the
settings tab does not change it for that run.

## 2. Kept settings

The terminal keeps its settings in the desktop's settings (libkeiland's
`kl_settings_*`), as the application's own keys, in
`~/.config/keiland/terminal.conf` (`name=value` lines). It opens no settings
file itself. A key at its default has no line.

| Key | Value | Default | Takes effect | Changed by |
| --- | --- | --- | --- | --- |
| `terminal.font-size` | pixels at 100 %, 8 to 32 | 16 | at once | View > Zoom, View > Text Size, the settings tab |
| `terminal.theme` | 0 dark, 1 light, 2 high contrast | 0 | at once | View > Theme, the settings tab |
| `terminal.ambiguous-wide` | 0 or 1: East Asian Ambiguous characters take two cells | 0 | at once | View > Treat Ambiguous-Width Characters as Wide (Alt+F11), the settings tab |
| `terminal.copy-on-select` | 0 or 1: the text the pointer selects is also put on the clipboard (it is always the primary selection) | 0 | at once | the settings tab |
| `terminal.font` | the font file's full path | none: `keiland-mono.ttf` | the next start | the settings tab |
| `terminal.fallback-font` | the fallback font file's full path | none: `keiland-fallback.ttf` | the next start | the settings tab |
| `terminal.columns`, `terminal.rows` | the grid the window opens with, 1 to 240 and 1 to 100 | 90, 28 | the next start | the settings tab |
| `terminal.term` | the `TERM` each tab's shell is given (letters, digits, `.`, `_`, `+`, `-`) | none: `xterm` | the next tab | the settings tab |

The windows and tabs of the terminal that changed a setting of "at once"
follow it at once; another terminal reads the settings when it starts. A
kept font that cannot be opened when the terminal starts (a file gone) gives
way to `keiland-mono.ttf`, and the log says `ZTERM FONT font=refused`.

## 3. The settings tab

Shell > Settings... (F12) opens the settings as a tab of their own beside the
others, or chooses it when it is open; the tab it was opened from stays, and
F12 on the settings tab goes back to it. There is one settings tab a window;
it is chosen and closed like any tab. The tab is a page of
libkeiland's widgets with a sidebar of four panes: Ctrl+Down shows the next
pane and Ctrl+Up the one before (the desktop's titlebar takes Ctrl+Page Up and
Ctrl+Page Down for the tabs), and Tab moves between a pane's controls.

| Pane | Setting | Takes effect | Kept in |
| --- | --- | --- | --- |
| Hosts | `~/.ssh/config`, a host at a time (below); Open in Text Editor (textedit) for the rest | the next `ssh` | `~/.ssh/config` |
| Appearance | text size (Smaller, Larger, Small to Huge), theme, ambiguous width | at once, as the View menu | `terminal.font-size`, `terminal.theme`, `terminal.ambiguous-wide` |
| Appearance | Font and Fallback font (a full path, or a file name in `/usr/share/fonts/`; checked to open as the terminal's font), Columns, Rows | the next start | `terminal.font`, `terminal.fallback-font`, `terminal.columns`, `terminal.rows` |
| Behavior | copy-on-select | at once | `terminal.copy-on-select` |
| Behavior | TERM | the next tab | `terminal.term` |
| Keys | a new `ssh-ed25519` key, a key's public key, `~/.ssh/known_hosts` | at once | `~/.ssh/` |

The Appearance and Behavior fields show the value in use (the default when
none is kept). Save keeps the fields that changed, in order; the first value
refused is named with why under the button and stops the save there (the
fields before it are kept); a font named without
a directory is kept as its full path; an empty field, or the default, takes
the kept value away. The card "From the next start" says the change waits for
the terminal to start again; "From the next tab" says the next tab's shell
takes it.

Hosts pane: the same as mikuterm's ([mikuterm](mikuterm.md), section 7):
each `Host` of `~/.ssh/config` in a form (Name, Host address, Port, User,
Command, Identity files, Port forwarding), only the lines a change is about
rewritten, Save (Ctrl+S) refused for a changed line the SSH client's reader
cannot use (a problem the file had already does not stop it) or a file
changed outside since it was read, Revert (Ctrl+R).
Open in Text Editor starts `textedit` with `~/.ssh/config` (made empty, mode
0600, when it is not there); the pane reads the editor's changes on Revert or
when the terminal starts again. The file is the one zedBSD's `ssh` reads: a
host's Port forwarding lines (`LocalForward`, `RemoteForward`) are set up by
each `ssh` to the host as its `-L` and `-R` are, and Command
(`RemoteCommand`) runs without a terminal unless `ssh` is given `-t` or the
host has `RequestTTY yes`, as with OpenSSH (mikuterm always gives it one).

Keys pane: the key's file name in `~/.ssh/` (`id_ed25519` by default). A key
of the name that is there shows its public key line and SHA256 fingerprint
with Copy Public Key; otherwise Comment (`user@host`), Passphrase and Again
make a new key with the system's random generator, written as `~/.ssh/NAME`
(OpenSSH's private key format, mode 0600; with a passphrase encrypted with
`aes256-ctr` under the `bcrypt` KDF, a 16-byte salt and 24 rounds) and
`~/.ssh/NAME.pub` (0644). `~/.ssh` is made 0700 when it is not there. A file
of either name that is there is never written over. Each line of
`~/.ssh/known_hosts` is listed with a button that removes it (asked once
more).

## 4. Tabs

| Action | Keys | |
| --- | --- | --- |
| New tab | Shell > New Tab, Ctrl+Shift+T, the titlebar's + | a shell, at the end |
| Close tab | Shell > Close Tab, Ctrl+Shift+W, the tab's close button | the shell is hung up; closing the last tab ends the terminal |
| Next, previous tab | Ctrl+Tab and Ctrl+Page Down, Ctrl+Shift+Tab and Ctrl+Page Up | the desktop's titlebar |
| Settings | Shell > Settings..., F12 | section 3 |

A window has at most ten tabs (Shell > New Tab does nothing past them). A
shell that exits closes its tab. Shift+Page Up and Shift+Page Down scroll the
tab's history. mikuterm has its own keys for the tabs on Windows
([mikuterm](mikuterm.md), section 5).

## 5. Menus

| Item | Shortcut |
| --- | --- |
| Shell > New Window | Ctrl+Shift+N (another terminal, on the same display) |
| Shell > New Tab, Close Tab, Close Window | Ctrl+Shift+T, Ctrl+Shift+W, Ctrl+Shift+Q |
| Shell > Settings... | F12 |
| Edit > Copy, Paste, Select All | Ctrl+Shift+C, Ctrl+Shift+V, Ctrl+Shift+A |
| Edit > Find..., Find Next, Find Previous | Ctrl+Shift+F, Ctrl+Shift+G, Ctrl+Shift+H |
| View > Zoom In, Zoom Out, Normal Size | Ctrl++, Ctrl+-, Ctrl+0 |
| View > Text Size, Theme | none |
| View > Fullscreen | F11 |
| View > Treat Ambiguous-Width Characters as Wide | Alt+F11 (kept in `terminal.ambiguous-wide` at once) |
| Session > Send Interrupt, Send End of File, Clear Screen, Reset Terminal | none |
| Help > About Terminal | none |

mikuterm has the same menus with the differences its reference lists
([mikuterm](mikuterm.md), section 8).

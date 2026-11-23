# mikuterm's shared files

mikuterm is the terminal of zedBSD (`userland/desktop/terminal`) with SSH tabs, as a program of
each system.  The files here are the ones every system's program shares: the settings and their
files, the connections and their threads, the launch screen, port forwarding, the settings tab,
and the X11 forwarding.  What speaks to one system (the entry point, the folder, the keeper of
passphrases, the start of the X server's process) is in that system's own directory
(`apps/terminal-win32`, `apps/terminal-linux`).

## The files

- `conf.c`, `settings-file.c`, `connection-ssh.c`, `known-hosts.c`, `ssh-thread.c`, `forward.c`,
  `remote-command.c`, `launch.c`, `page.c`: used by every program.
- `x11-link.c`, `x11-host.c`, `x11-serve.c`, `x11-tab.c`, `x11-noglx.c`: X11 forwarding, for a
  program that has it.  `x11-none.c` answers the same calls for a program that has none.
- `proxy-command.c`, `codespace.c`: starting programs of the computer's for ssh_config's
  ProxyCommand and for GitHub Codespaces, for a program that can start them.
- `wsl-none.c`: answers what the program asks of WSL, for a system that has none.
- The headers of these files, and `mikuterm-version.h.in`, the template of the version header.
- `cmake/mikuterm-sources.cmake`: the lists a CMake build reads (names below) and
  `mikuterm_read_sources()`, which reads a source list of the tree's `sources.mk`.  The build
  sets `MIKUTERM_ROOT` first.

## What each program reads

| list | Windows | Linux | no WSL, no X11 |
| --- | --- | --- | --- |
| `MIKUTERM_COMMON_SOURCES` | yes | yes | yes |
| `MIKUTERM_COMMON_X11_SOURCES` | yes | yes | |
| `MIKUTERM_COMMON_PROGRAM_SOURCES` | yes | unless Flatpak | |
| `MIKUTERM_COMMON_NO_WSL_SOURCES` | | yes | yes |
| `MIKUTERM_COMMON_NO_X11_SOURCES` | | | yes |

`MIKUTERM_TERMINAL_SOURCES` and `MIKUTERM_SSH_SOURCES` are the terminal's and the SSH library's
modules that every program uses.  Every program adds this directory to its include path.

## The features

A program says what it has with a definition of 1 or 0 when it compiles.  The files that look at
`MIKUTERM_HAVE_WSL` and `MIKUTERM_HAVE_OPENER` stop with an error when it is not defined; the
last three are 0 when not defined (`mikuterm.h`; the host tests have none of them).

| definition | meaning |
| --- | --- |
| `MIKUTERM_HAVE_WSL` | the launch screen has a WSL row, and WSL's host is asked for |
| `MIKUTERM_HAVE_OPENER` | a settings file can be opened in another program (`xdg-open`, Notepad) |
| `MIKUTERM_HAVE_X11` | X11 forwarding with the program's own X server (the build's choice of files; no C file reads it yet) |
| `MIKUTERM_HAVE_LOGIN_SHELL` | the launch screen's Login Shell |
| `MIKUTERM_HAVE_PROXY_COMMAND` | ssh_config's ProxyCommand starts a program of the computer's |
| `MIKUTERM_HAVE_CODESPACES` | GitHub Codespaces, reached through gh as a ProxyCommand |

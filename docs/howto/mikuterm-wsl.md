# Connecting mikuterm to WSL

Status: how-to; mikuterm on Windows 10 1903 or later or Windows 11, WSL 2
with an Ubuntu distribution

mikuterm ([reference](../reference/mikuterm.md)) reaches WSL the way it
reaches any host: through an SSH server running inside WSL. It does not use
WSL's own interfaces; it only asks `wsl.exe` for the list of distributions
and starts the one chosen. WSL 2 forwards a port opened inside WSL to the
same port on Windows' `localhost`, so mikuterm connects to `localhost` and a
port of its own (2222, so that it does not meet a server on Windows' port 22).

## Prerequisites

- mikuterm unpacked into a folder you can write in.
- A WSL 2 distribution (`wsl --list --verbose` in PowerShell shows `VERSION 2`).
- An ed25519 key. To make one in PowerShell (it asks for a passphrase; an
  empty one makes a key without):

  ```
  ssh-keygen -t ed25519 -f id_ed25519
  ```

  Put `id_ed25519` into mikuterm's `ssh\` folder. Keep `id_ed25519.pub` for
  the next step.

## 1. The SSH server in WSL

In the WSL shell:

```
sudo apt update
sudo apt install openssh-server
sudo sed -i 's/^#\?Port .*/Port 2222/' /etc/ssh/sshd_config
mkdir -p ~/.ssh
chmod 700 ~/.ssh
cat /mnt/c/path/to/id_ed25519.pub >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys
```

Start the server:

- With systemd (the default of recent WSL and Ubuntu):
  `sudo systemctl enable --now ssh`. Ubuntu 22.10 and later start sshd from
  `ssh.socket`; after changing the port run `sudo systemctl daemon-reload`
  and `sudo systemctl restart ssh.socket`.
- Without systemd: `sudo service ssh start` (again after each start of WSL).

Check it listens: `ss -ltn | grep 2222` shows a line with `:2222`.

## 2. The distribution in mikuterm

Nothing is needed for one distribution whose sshd listens on 2222: mikuterm
connects to `localhost` on `WslPort` (2222 unless `config\mikuterm.conf` or
the settings' Behavior pane says otherwise) as the distribution's default
user.

All the distributions of WSL 2 share one network, so two cannot listen on the
same port. For a second distribution, give its sshd another port (2223) and
add a Host named `wsl-` and the distribution's name to mikuterm's
`ssh\config`:

```
Host wsl-Debian
	HostName localhost
	Port 2223
```

A `User` there is used instead of the distribution's default user.

To open a distribution in the first tab whenever mikuterm starts, give it such
a Host and add to `config\mikuterm.conf`:

```
Connect wsl-Ubuntu-24.04
```

## 3. The first connection

Start `mikuterm.exe`. On the tab's screen press `w` (or choose `[w] WSL` with
Up and Down) and Enter: mikuterm lists the distributions `wsl -l -q` lists.

```
WSL distributions:
 [1] Ubuntu-24.04  (localhost:2222)
 [2] Debian  (ssh\config: wsl-Debian)
 [b] Back
```

Choose one and press Enter. mikuterm starts the distribution (and with it an
sshd systemd runs), then connects. The first time, it shows the server's
key:

```
The authenticity of host '[localhost]:2222 ([127.0.0.1]:2222)' can't be established.
ED25519 key fingerprint is SHA256:...
This key is not known by any other names.
Are you sure you want to continue connecting (yes/no/[fingerprint])?
```

Compare it with the key in WSL:

```
ssh-keygen -l -f /etc/ssh/ssh_host_ed25519_key.pub
```

and type `yes` when they are the same.

**Success:** the tab shows `Warning: Permanently added '[localhost]:2222'
(ED25519) to the list of known hosts.` and WSL's shell prompt;
`ssh\known_hosts` has a line starting with `[localhost]:2222 ssh-ed25519`.
The next connection goes straight to the prompt.

## When it does not work

| The tab shows | Cause and remedy |
| --- | --- |
| `WSL is not installed: Windows has no wsl.exe.` | WSL is not enabled on this Windows: `wsl --install` in an administrator's PowerShell. |
| `WSL: ...` or `WSL has no distribution` | wsl.exe's own words: install a distribution (`wsl --install -d Ubuntu-24.04`). |
| `ssh: connect to host localhost port 2222: Connection refused` (or `Connection timed out`), then `NAME needs an sshd listening on port 2222` | sshd is not running in the distribution, or it listens on another port: open the WSL shell, start sshd (step 1) and check `ss -ltn` there; or set `WslPort`, or the distribution's Host, to the port it listens on. If WSL's networking mode is not the default NAT mode, check that its localhost forwarding is on (`localhostForwarding` in `.wslconfig`). |
| `me@localhost's password:` | The key was not accepted: check `~/.ssh/authorized_keys` (the `.pub` line, mode 600) and that `ssh\id_ed25519` is the matching private key. |
| `WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!` | WSL's host keys were made again (a reinstalled distribution): delete the line the warning names from `ssh\known_hosts` and connect again. |
| `ssh\config: line N: ...` | A keyword mikuterm does not support (for example `Include`): remove the line. |

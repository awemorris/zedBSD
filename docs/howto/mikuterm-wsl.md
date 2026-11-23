# Connecting mikuterm to WSL

Status: how-to; mikuterm on Windows 10 1903 or later or Windows 11, WSL 2
with an Ubuntu distribution

mikuterm ([reference](../reference/mikuterm.md)) reaches WSL the way it
reaches any host: through the SSH server of the distribution, on its own
port 22. It does not use WSL's own interfaces; it only asks `wsl.exe` for the
list of distributions, starts the one chosen and asks it for its address
(`wsl.exe -d <name> -e hostname -I`, as Microsoft's WSL documentation
gives). With WSL's default networking (NAT) the distribution has an address
of its own, which Windows reaches directly; with mirrored networking it
shares Windows' addresses and is reached at `localhost`.

## Prerequisites

- mikuterm unpacked into a folder you can write in.
- A WSL 2 distribution (`wsl --list --verbose` in PowerShell shows `VERSION 2`).
- An ed25519 key. To make one in PowerShell (it asks for a passphrase; an
  empty one makes a key without):

  ```
  ssh-keygen -t ed25519 -f id_ed25519
  ```

  Put `id_ed25519` into mikuterm's `ssh/` folder. Keep `id_ed25519.pub` for
  the next step.

## 1. The SSH server in WSL

In the WSL shell:

```
sudo apt update
sudo apt install openssh-server
mkdir -p ~/.ssh
chmod 700 ~/.ssh
cat /mnt/c/path/to/id_ed25519.pub >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys
```

Start the server:

- With systemd (the default of recent WSL and Ubuntu):
  `sudo systemctl enable --now ssh`.
- Without systemd: `sudo service ssh start` (again after each start of WSL).

Check it listens: `ss -ltn | grep ':22 '` shows a line with `:22`.

## 2. The distribution in mikuterm

Nothing is needed: mikuterm connects to the distribution's address on
`WslPort` (22 unless `config/mikuterm.conf` or the settings' Behavior pane
says otherwise) as the distribution's default user, and keeps its host key
in `ssh/known_hosts` under the name `wsl-<name>`.

Another port is needed in two cases:

- Mirrored networking (`networkingMode=mirrored` in `.wslconfig`) while
  Windows' own OpenSSH Server runs: the distribution shares Windows' ports,
  and Windows' sshd has 22. mikuterm says so instead of connecting.
- Two distributions that both run an sshd: all the distributions of WSL 2
  share one network, so they cannot both listen on 22.

Then give the distribution's sshd another port (2222 here):

```
sudo sed -i 's/^#\?Port .*/Port 2222/' /etc/ssh/sshd_config
sudo systemctl daemon-reload
sudo systemctl restart ssh.socket ssh
```

and either set `WslPort 2222` in `config/mikuterm.conf`, or add a Host named
`wsl-` and the distribution's name to mikuterm's `ssh/config` (no HostName:
mikuterm finds the address each time):

```
Host wsl-Debian
	Port 2222
```

A `User` there is used instead of the distribution's default user. To open a
distribution in the first tab whenever mikuterm starts, give it such a Host
and add `Connect wsl-<name>` to `config/mikuterm.conf`.

## 3. The first connection

Start `mikuterm.exe`. On the tab's screen press `w` (or choose `[w] WSL` with
Up and Down) and Enter: mikuterm lists the distributions `wsl -l -q` lists.

```
WSL distributions:
 [1] Ubuntu-24.04  (its sshd, port 22)
 [2] Debian  (ssh/config: wsl-Debian)
 [b] Back
```

Choose one and press Enter. mikuterm starts the distribution (and with it an
sshd systemd runs), asks it for its address, then connects. The first time, it shows the server's
key:

```
The authenticity of host 'wsl-ubuntu-24.04 (172.30.98.229)' can't be established.
ED25519 key fingerprint is SHA256:...
This key is not known by any other names.
Are you sure you want to continue connecting (yes/no/[fingerprint])?
```

Compare it with the key in WSL:

```
ssh-keygen -l -f /etc/ssh/ssh_host_ed25519_key.pub
```

and type `yes` when they are the same.

(the address is the distribution's, `127.0.0.1` with mirrored networking).

**Success:** the tab shows `Warning: Permanently added 'wsl-ubuntu-24.04'
(ED25519) to the list of known hosts.` and WSL's shell prompt;
`ssh/known_hosts` has a line starting with `wsl-ubuntu-24.04 ssh-ed25519`.
The next connection goes straight to the prompt, though WSL gives the
distribution another address each time it starts.

## When it does not work

| The tab shows | Cause and remedy |
| --- | --- |
| `WSL is not installed: Windows has no wsl.exe.` | WSL is not enabled on this Windows: `wsl --install` in an administrator's PowerShell. |
| `WSL: ...` or `WSL has no distribution` | wsl.exe's own words: install a distribution (`wsl --install -d Ubuntu-24.04`). |
| `ssh: connect to host ADDRESS port 22: Connection refused`, then `Nothing listens on port 22 of NAME (ADDRESS): its sshd is not installed, not started, or on another port ...` | sshd is not installed or not running in the distribution, or it listens on another port: open the WSL shell, install and start sshd (step 1) and check `ss -ltn` there; or set `WslPort`, or the distribution's Host, to the port it listens on. |
| `ssh: connect to host ADDRESS port 22: Connection timed out`, then `NAME did not answer at ADDRESS port 22: WSL's network or a firewall keeps Windows from it ...` | Windows cannot reach the distribution's address: check WSL's networking (`wsl --shutdown` and start again), and that no firewall rule blocks Windows' traffic to WSL. |
| `Windows' OpenSSH Server listens on port 22, so NAME's sshd cannot be reached there ...` (on the WSL screen, nothing connected) | Mirrored networking: the distribution shares Windows' ports and Windows' own sshd has 22. Give the distribution's sshd another port (step 2), or stop Windows' OpenSSH Server if you do not use it. The same line names any other program of Windows that has the port. |
| `Note: a program of Windows also listens on localhost port 22 ...` before the key's question | mikuterm could not tell which program it is. If the key asked about is not the distribution's (compare with step 3), answer no and give the distribution another port (step 2). |
| `me@ADDRESS's password:` | The key was not accepted: check `~/.ssh/authorized_keys` (the `.pub` line, mode 600) and that `ssh/id_ed25519` is the matching private key. |
| `WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!` | WSL's host keys were made again (a reinstalled distribution): delete the line the warning names from `ssh/known_hosts` and connect again. |
| `ssh/config: line N: ...` | A keyword mikuterm does not support (for example `Include`): remove the line. |

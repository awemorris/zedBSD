# Connecting mikuterm to WSL

Status: how-to; mikuterm on Windows 10 1903 or later or Windows 11, WSL 2
with an Ubuntu distribution

mikuterm ([reference](../reference/mikuterm.md)) reaches WSL the way it
reaches any host: through an SSH server running inside WSL. It does not use
WSL's own interfaces. WSL 2 forwards a port opened inside WSL to the same
port on Windows' `127.0.0.1`, so mikuterm connects to `127.0.0.1` and a port
of its own (2222 here, so that it does not meet a server on Windows' port 22).

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

## 2. The host in mikuterm

Add to mikuterm's `ssh\config` (your WSL user name for `me`):

```
Host wsl
	HostName 127.0.0.1
	Port 2222
	User me
```

To open WSL in the first tab whenever mikuterm starts, add to
`config\mikuterm.conf`:

```
Connect wsl
```

## 3. The first connection

Start `mikuterm.exe`, open a tab and answer `Host:` with `wsl` (or its
number). The first time, mikuterm shows the server's key:

```
The authenticity of host '[127.0.0.1]:2222 ([127.0.0.1]:2222)' can't be established.
ED25519 key fingerprint is SHA256:...
This key is not known by any other names.
Are you sure you want to continue connecting (yes/no/[fingerprint])?
```

Compare it with the key in WSL:

```
ssh-keygen -l -f /etc/ssh/ssh_host_ed25519_key.pub
```

and type `yes` when they are the same.

**Success:** the tab shows `Warning: Permanently added '[127.0.0.1]:2222'
(ED25519) to the list of known hosts.` and WSL's shell prompt; `ssh\known_hosts`
has a line starting with `[127.0.0.1]:2222 ssh-ed25519`. The next connection
goes straight to the prompt.

## When it does not work

| The tab shows | Cause and remedy |
| --- | --- |
| `ssh: connect to host 127.0.0.1 port 2222: Connection refused` (or `Connection timed out`) | WSL is not running, sshd is not running in it, or it listens on another port: open the WSL shell, start sshd (step 1) and check `ss -ltn` there. If WSL's networking mode is not the default NAT mode, check that its localhost forwarding is on (`localhostForwarding` in `.wslconfig`). |
| `me@127.0.0.1's password:` | The key was not accepted: check `~/.ssh/authorized_keys` (the `.pub` line, mode 600) and that `ssh\id_ed25519` is the matching private key. |
| `WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!` | WSL's host keys were made again (a reinstalled distribution): delete the line the warning names from `ssh\known_hosts` and connect again. |
| `ssh\config: line N: ...` | A keyword mikuterm does not support (for example `Include`): remove the line. |

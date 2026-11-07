# Session Management and Graphical Login

Status: current

This document describes the session architecture of zedBSD, the operation of
the session manager (`sessiond`), user authentication, desktop lifecycle, and
automatic fallback mechanisms.

---

## 1. Architectural Role of `sessiond`

In zedBSD, [`sessiond`](../../userland/desktop/sessiond/main.c) is the system's
graphical session manager. It runs as root and replaces the traditional text
console `getty` when graphical login is enabled (`login=graphical` in boot
parameters).

### Design Principles

1. **Zero Drawing / Minimal Attack Surface**:
   `sessiond` does not render pixels, decode image files, or parse TrueType fonts.
   All graphical rendering is delegated to the unprivileged greeter or user
   compositor.
2. **Strict Privilege Separation**:
   - The login greeter runs under a dedicated, unprivileged system account (`_greeter`).
   - The user desktop session runs strictly under the authenticated user's UID/GID.
   - `sessiond` retains root privileges only to manage PAM/shadow authentication,
     allocate terminal/seat devices, adjust process limits, and orchestrate
     power management actions.
3. **Deterministic Fallback**:
   If graphical display hardware fails, crashes repeatedly, or cannot be claimed,
   `sessiond` restores the text console and drops into a text login `getty`.

---

## 2. Session Lifecycle and State Machine

```
              +------------------------------------+
              |          System Boot (init)        |
              +------------------------------------+
                                |
                                v
              +------------------------------------+
              |           Start sessiond           |
              +------------------------------------+
                                |
             +------------------+------------------+
             |                                     |
    [Autologin Configured]                [Standard Boot]
             |                                     |
             v                                     v
+--------------------------+          +--------------------------+
|  User Session (Bypass)   |          |  Greeter Phase           |
|  /etc/keiland/session    |          |  /bin/wayland --greeter  |
+--------------------------+          +--------------------------+
             |                                     |
             |                                     | Authentication
             |                                     | Success
             |                                     v
             |                        +--------------------------+
             +----------------------->|  User Desktop Session    |
                                      |  /etc/keiland/session    |
                                      +--------------------------+
                                                   |
                         +-------------------------+-------------------------+
                         |                                                   |
                 [Clean Logout]                                       [Session Crash]
                         |                                                   |
                         v                                                   v
            +--------------------------+                        +--------------------------+
            |  Restart Greeter Phase   |                        |  Immediate Restart       |
            |  (Return to Login Screen)|                        |  (Restore Screen Lock)   |
            +--------------------------+                        +--------------------------+
                                                                             |
                                                                (>= 3 crashes in 2 mins)
                                                                             |
                                                                             v
                                                                +--------------------------+
                                                                |  Fallback to Greeter     |
                                                                +--------------------------+
```

---

## 3. The Greeter Phase

When no automatic login is configured, `sessiond` initializes the greeter:

1. **Display Detection**:
   Checks whether `/dev/gpu0` is available. If the kernel indicates an ongoing
   GPU probe (`hw.gpu.attaching` sysctl), `sessiond` waits for up to 10 seconds
   for driver initialization to complete.
2. **Process Spawning**:
   `sessiond` creates a connected socketpair (`auth_fd`), drops privileges to the
   `_greeter` user, and executes `/bin/wayland --greeter --auth-fd=<fd>`.
3. **Authentication Exchange**:
   - The greeter presents the list of users, avatar icons, and password fields.
   - When the user submits credentials, the greeter sends an authentication request
     message over `auth_fd`.
   - `sessiond` verifies the password against `/etc/shadow` using the system
     authentication routines.
   - Upon verification, `sessiond` sends an approval response, terminates the
     greeter process, and releases the seat.

---

## 4. The User Session Phase

Once authenticated, `sessiond` launches the user's desktop environment:

1. **Environment & Seat Setup**:
   - Initializes environment variables (`HOME`, `USER`, `LOGNAME`, `SHELL`,
     `XDG_RUNTIME_DIR`).
   - Ensures `$XDG_RUNTIME_DIR` exists (`/run/user/<uid>`) with mode `0700` and
     user ownership.
   - Opens an IPC control descriptor (`control_fd`) to monitor the session's readiness.
2. **Executing the Session Script**:
   - Switches UID, GID, and supplementary groups to the authenticated user.
   - Executes `/etc/keiland/session`, which starts `/bin/wayland --session --control-fd=<fd>`
     alongside the desktop panel, notification daemon, and background services.
3. **Readiness Acknowledgment**:
   - The user compositor signals `READY` over `control_fd` once its display pipelines
     are established.
   - If the session fails to report readiness within 60 seconds, `sessiond` considers
     the startup failed and triggers fallback recovery.

---

## 5. Automatic Login (`autologin`)

zedBSD supports zero-interaction graphical login for embedded devices, kiosks,
or single-user workstations:

- **Configuration**:
  Specified in `/etc/keiland/autologin` containing the target username.
- **Boot Flow**:
  `sessiond` directly reads `/etc/keiland/autologin`. If the user exists and the
  account is valid, `sessiond` skips the greeter phase and immediately launches
  `/etc/keiland/session` for that user.
- **Fail-safe**:
  If the automatically logged-in session fails to claim the display or crashes
  within 60 seconds, automatic login is aborted and the system reverts to the
  standard greeter phase.

---

## 6. Fault Tolerance and Text Console Fallback

A key feature of `sessiond` is its ability to handle driver crashes or corrupt
display states without bricking the operating system.

### 6.1 Retry Rules & Exponential Backoff

If the greeter or user session exits unexpectedly before taking over the display,
`sessiond` applies the rules defined in `fallback-rules.c`:
- **Backoff Interval**: The delay between retry attempts increases exponentially
  from 1 second up to 16 seconds (giving hardware and GPU firmware time to recover).
- **Max Retries**: A maximum of 6 consecutive failures is permitted.
- **Maximum Time Horizon**: If 90 seconds elapse in continuous failure states,
  retries are halted.

### 6.2 Restoring the Text Console (`/dev/system`)

When `sessiond` exhausts its retry limit or detects that no display is present:
1. It issues a restore ioctl to `/dev/system`.
2. The kernel ends display takeover, unbinds the GPU lease, clears any stuck boot
   splash, and restores kernel console messaging to the framebuffer or VGA text console.
3. `sessiond` outputs a clear diagnostic line explaining why the graphical login
   was abandoned (e.g. `sessiond: graphical login unavailable: no display device`).
4. `sessiond` exits with code 0.
5. In response, `init` restarts the console `getty` service, providing the user
   with an interactive text login prompt.

### 6.3 User Session Crash Recovery

If an active user session crashes after it has successfully been running:
- `sessiond` treats it as a transient fault, not a failed boot.
- It immediately relaunches the session for the **same user** without showing
  the greeter.
- The session is started in locked state (`--locked`), preserving the user's
  security.
- If 3 crashes occur within a rolling 2-minute window, `sessiond` abandons
  the session and returns to the login greeter.

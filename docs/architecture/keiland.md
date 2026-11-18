# Keiland desktop environment

This document describes Keiland from the outside: what it is for, how it is
divided, which interfaces are stable, and what a user and an application see.
It does not describe internal implementation.

## Purpose

Keiland exists to build a new kind of desktop: one that joins a touch UI/UX
and the conventional desktop UI/UX into one environment. Commercial desktops
such as macOS and Windows have not achieved this. They either keep a
mouse-and-keyboard desktop with touch added on top, or switch to a separate
tablet mode. Keiland treats a finger, a pen, a mouse and a keyboard as equal
ways to use the same windows, menus and system controls.

Two design choices follow from this aim and run through the rest of the
document:

- **One desktop, not a set of cooperating programs.** Parts that other
  environments run as separate processes, such as the on-screen keyboard, are
  part of the compositor. The desktop is meant to feel like one thing.
- **Portability is a design goal, not an afterthought.** Keiland runs as the
  native desktop of zedBSD and is also ported to Windows, Linux, and FreeBSD.
  Everything that differs between operating systems is kept behind defined boundaries.
  The client toolkit (`libkeiland`) is decoupled from the compositor, enabling
  applications like the Keiland Browser to run natively on Windows as well as
  Unix platforms.

## Desktop modes

Keiland has two desktop modes. The one being built now is the tablet mode;
the phone mode is planned.

**Tablet mode** is for tablets, laptops and desktops with a screen of about
7 inches or more.

- A window can move between maximized and windowed without a break.
- When a window is maximized, its titlebar docks into the dock bar (the
  system bar at the top of the screen).
- The UI is designed for a touch screen first, and it can also be used with
  a trackpad, a mouse and a keyboard.

**Phone mode** is for touch screens up to about 6 inches.

- Only one application is shown at a time, always maximized.
- Several windows cannot be shown side by side.
- There is a dock bar, but it is too narrow to show the application's menus.

**Game mode** is for fullscreen games, emulators, and high-performance 3D applications.

- The application renders directly via native **Vulkan**.
- When fullscreen, the compositor engages **direct scanout**: the application's
  Vulkan swapchain buffer is assigned directly to the display scanout hardware,
  bypassing compositor rendering passes entirely.
- Delivers zero compositing latency, optimal frame pacing, and hardware Variable
  Refresh Rate (VRR) support.

### Application lifecycle messages

The compositor and applications exchange these messages, so that the same
application can run in both modes.

From the compositor to an application:

- The application goes to the background. Its process is stopped.
- The application comes to the foreground. Its process runs again.
- The application is terminated, by the out-of-memory killer or by a manual
  kill.
- The screen size changes, for example when an external display is connected
  or a foldable display is opened or closed.

From an application to the compositor:

- Which modes the application can run in: phone mode, tablet mode, or both.
- A request to keep running while in the background. This is honoured only in
  tablet mode.

## Layers

| Layer | What it is | Who uses it | Stability |
| --- | --- | --- | --- |
| Applications | Programs with windows: Files, Settings, Terminal, Notes, the browser and others, plus X11 programs through the X server | The user | — |
| [libkeiland](../../userland/desktop/include/keiland/keiland.h) | The public C library of the desktop | Applications | **Stable public API** (`KEILAND_VERSION`) |
| Standard Wayland and Vulkan | Core Wayland, `xdg-shell` and other standard protocols; Vulkan for drawing | Applications | Upstream standards |
| Keiland's own Wayland extensions | `keiland_*_v1` protocols spoken between libkeiland and the compositor | libkeiland only | **Internal**; may change at any time |
| Keiland compositor | The Wayland compositor: windows, input, system bar, on-screen keyboard, input method host | — | — |
| [libkeiland-backend](../../userland/desktop/libkeiland-backend/keiland-backend.h) | Everything in the compositor that differs between operating systems | The compositor only | Internal, compile-time interface |
| Operating system | zedBSD, Linux, FreeBSD, or Windows: kernel, GPU driver, network and sound services | The backend | — |

## Wayland compositor and protocols

Keiland is a Wayland compositor. It speaks the standard protocols an ordinary
Wayland client expects: core Wayland (`wl_compositor`, `wl_shm`, `wl_seat`,
`wl_data_device_manager`, subsurfaces), `xdg-shell` with popups and
positioners, server-side decoration (`zxdg_decoration_manager_v1`), cursor
shapes, primary selection, the tablet protocol (`zwp_tablet_manager_v2`),
`zwp_text_input_v3`, the input method and virtual keyboard protocols for
the system's input method, and `xdg_activation_v1` (a program brings its
window, or a running copy's window, to the front with a token the desktop
gave it when it started it). An unmodified GTK, Qt or SDL client runs as a
normal Wayland window.

The parts of the UI/UX that only Keiland has are carried by its own
extensions, for example:

| Extension | What it carries |
| --- | --- |
| `kl_titlebar_manager_v1` | The window's titlebar presentation: menu, controls (navigation, breadcrumb, search, view selector) or tabs |
| `xdg_menu_manager_v1` | The application's menus as a tree of items, drawn by the compositor in the titlebar or the system bar |
| `kl_glass_manager_v1` | Which parts of a surface stand on the desktop's frosted glass, and whether the glass blurs what is under it |
| `kl_desktop_manager_v1` | The desktop surface (the icons of `~/Desktop`) between the wallpaper and the windows |
| `kl_edit_manager_v1` | Editing operations (copy, paste, select all) that the system UI can ask a window to perform |
| `kl_keyboard_inset_manager_v1` | How much of a window the on-screen keyboard covers, so the window can keep the caret in view |
| `kl_ime_status_manager_v1` | The input method's state (language, mode) for the system bar's indicator, and the on-screen keyboard's predictions: the compositor asks for the words a reading starts and has the word chosen learned (version 2) |
| `kl_theme_v1` | The desktop's appearance, light or dark: told when a client binds it and again whenever the user changes it, so that applications draw in it and redraw when it changes |

**These extensions are not an application interface.** An application never
binds them itself. libkeiland wraps every one of them, and the protocols are
an internal implementation detail between libkeiland and the compositor: a
protocol can be renamed, versioned or replaced in any release, as long as the
libkeiland API it serves keeps its meaning.

## libkeiland: the stable public API

[libkeiland](../../userland/desktop/include/keiland/keiland.h) is the one interface an
application uses to reach anything Keiland offers beyond standard Wayland and
Vulkan. Its API is public and stable. `KEILAND_VERSION` (21 at the time of
writing) increases when calls are added, and `keiland_version()` reports the
version of the library actually loaded, so a program can detect an older
library.

The API covers the UI and also settings of the whole system. Its families
include:

| Family | Examples |
| --- | --- |
| Window UI | System Menu (`keiland_menu_*`), titlebar presentation (`keiland_titlebar_*`, including sheets), context menus, glass panels, the desktop surface, window operations |
| Appearance | The desktop's appearance, light or dark (`kl_theme_*`): the colours of the current theme, and a notice to redraw when the user changes it, carried by `kl_theme_v1` |
| Touch | The touch motion, the scroller and the gestures (`keiland_motion_*`, `keiland_scroller_*`, `keiland_gesture_*`), so that a finger feels the same in every program |
| Input | The keyboard inset of the on-screen keyboard, editing operations |
| Widgets | The widgets' input (`kl_ui_*`): each frame records the parts that take input. A change of the widget under the pointer names only the widget lit when the last frame was drawn and the one lit now, however many the pointer crossed in between; with the application's own changes (`kl_ui_damage`) they are taken as a few parts of the window to draw and show (`kl_ui_take_frame`, `kl_window_present_parts`). A part that draws the same with the pointer on it, such as a picture a press drags, is recorded with `kl_ui_hit_still` and asks for no drawing |
| System | The network, including Wi-Fi control, saved keys, links and DNS (`keiland_network_*`); the sound output's volume and mute (`keiland_audio_*`); the desktop's preferences; recent files |

A call is handled in one of two places, and the application does not need to
know which:

- **In libkeiland itself**, for example the touch motion and scroller
  calculations, or reading the desktop's preferences.
- **Bridged to the compositor** through the internal extensions, for example
  a menu the compositor draws, or a system setting the compositor owns.

How a system call is carried out differs per operating system (for example
how Wi-Fi is controlled on zedBSD, Linux and FreeBSD). That difference stays
below libkeiland; the call's meaning does not change.

## libkeiland-backend: the operating-system boundary

Existing Wayland compositors are tightly bound to Linux interfaces (DRM/KMS,
dma-buf, logind, udev). Porting one to another POSIX system usually means
forking it. Keiland instead puts every operating-system dependency of the
compositor into a backend, `libkeiland-backend`, with one interface
([keiland-backend.h](../../userland/desktop/libkeiland-backend/keiland-backend.h))
and one implementation per system:

- `libkeiland-backend-zedbsd`
- `libkeiland-backend-linux`
- `libkeiland-backend-freebsd`

The compositor calls only this interface; an area a system does not offer
answers `ENOTSUP`. Applications never see the backend.

GPU buffers show why the split matters. On Linux, buffers are shared through
dma-buf. On zedBSD, the GPU driver offers a much simpler buffer model, and the
compositor uses that directly. With both behind the backend, the same
compositor is a complete, maintainable desktop on Linux and on a non-Linux
POSIX system such as FreeBSD, not a Linux program with a compatibility layer.

The backend covers every area where the systems differ: the network, sound,
power, the seat and session, input devices, the display and the GPU buffers.

## Touch input, on-screen keyboard and input method

The on-screen keyboard is drawn and controlled by the compositor itself. A
swipe from a bottom corner opens it: the QWERTY panel along the bottom from
the bottom-left corner, the flick panel at the side from the bottom-right
corner. Desktops such as GNOME run the on-screen keyboard as a separate
process. Keiland deliberately does not: it chooses a desktop that feels like
one piece over process separation, so that the keyboard, the windows and the
gestures share one notion of where a finger is.

The input method is one program (`keiland-ime`) that the compositor starts
and supervises. It speaks the standard `zwp_input_method_v2` and virtual
keyboard protocols, and applications receive text through the standard
`zwp_text_input_v3`. Standard Wayland input methods are built around a
hardware keyboard. They make touch input hard to do well, in particular
predictive conversion while typing on the on-screen keyboard. Keiland
therefore extends its input method beyond the Wayland standard. The input
method talks to the compositor's on-screen keyboard through Keiland's own
extensions, so prediction and the keyboard work together. These extensions
are internal, like the others.

## Titlebar and system bar

The titlebar is Keiland's most visible feature.

- **Server-side decoration.** The compositor draws every window's titlebar.
  A client that asks for client-side decoration is honored, and a full-screen
  window has no titlebar.
- **One integrated bar.** The title, the application's menus, a search field
  and the application's buttons share the titlebar. The application gives
  their meaning through libkeiland. The compositor decides the layout, so
  menus and controls look and behave the same in every program. When room
  runs short, controls fold into a "..." popup in a defined order.
- **Floating and touchable.** The titlebar floats over the window as a
  rounded glass card, sized for a finger as well as a pointer.
- **Docking into the system bar.** When a window is maximized, its titlebar
  merges into the system bar at the top of the screen. The window's menus
  and controls appear in the system bar's application zone, so a small
  screen keeps its full height for the content.

The system bar also holds the system's own controls (network, sound, battery,
input method, virtual desktops). They are driven by the same compositor and
reached by applications such as Settings through libkeiland.

## Login, sessions and the lock screen on zedBSD

On zedBSD the graphical login and the sessions are run by `sessiond`
([userland/desktop/sessiond](../../userland/desktop/sessiond/)), a small
daemon that runs as root and is started by init in place of the console
getty. It is the only part of the desktop that holds privilege; it has every
password, PIN and security key checked by `/sbin/passkey`
([Login authentication](security.md#login-authentication)). Linux and FreeBSD keep their own display managers and session
services; the backend talks to them instead.

### The greeter

The greeter is not a separate program. It is the Keiland compositor
(`/bin/wayland`) started with `--greeter`, so the login screen looks and
behaves like the desktop. sessiond starts it like this:

1. It creates a socket pair and forks.
2. The child starts its own session (`setsid`). It drops to the `_greeter`
   account (`initgroups`, then `setgid`, then `setuid`; the home is
   `/var/empty`, the shell `nologin`). It closes every descriptor of
   sessiond's except its end of the socket pair, which becomes descriptor 3.
   It sets a minimal environment (`USER`, `HOME`, `PATH`) and executes
   `/bin/wayland --greeter --auth-fd=3`.
3. The greeter draws the login screen and talks to sessiond on descriptor 3,
   one line per request:

   | Request | Answer |
   | --- | --- |
   | `READY` | `GO` once nothing else holds the display |
   | `STYLES name` | `STYLES password[ pin][ fido2]`: the ways the account can log in now |
   | `AUTH name style`, then the secret on the next line | `TOUCH` while a security key waits to be touched, then `OK`, or `FAIL reason` after a delay |
   | `CANCEL` | stops a security key attempt under way (its answer is `FAIL timeout`) |
   | `POWER poweroff` / `POWER reboot` | `OK`, then the machine ends |

   Anything else is answered `ERROR`, and a request that comes while another
   is answered (but `CANCEL`) is answered `ERROR busy`. The line after `AUTH`
   is always the secret, never a request. The reasons of `FAIL` are
   `bad-secret`, `locked`, `pin-off`, `no-key`, `timeout`, `device` and
   `busy`.

The greeter has no privilege. It cannot read `/etc/shadow`, `/etc/passkey` or
any user's home directory, and it is not given the security keys' device
nodes; it only passes what the user typed to sessiond.

### Checking a password, a PIN or a security key

sessiond does not check credentials itself. It runs `/sbin/passkey` for every
attempt and keeps the failure counts in memory
([Login authentication](security.md#login-authentication)). For a password,
passkey uses the same routine as the console `login`
([verify.c](../../userland/base/login/verify.c)):

1. Look the account up in `/etc/passwd`, then its hash in `/etc/shadow`
   (readable only by root).
2. A hash starting with `!` or `*` is a locked account and takes no password.
   An empty hash takes only an empty password.
3. Otherwise hash the password with `crypt()` and the stored salt (SHA-512
   crypt), and compare.
4. Erase the password, whatever the answer.

After a wrong password, PIN or key the answer waits: 2 seconds, doubling after
every three failures in a row, up to 16 seconds. Five wrong PINs in a row turn
the PIN off until the next password or security key login. Every success and
failure is logged to syslog, and no secret is ever written anywhere.

### Starting the session

After `OK`, sessiond starts the user's session as that user: another
compositor with a control socket pair as descriptor 3. The greeter keeps the
screen ("Starting session") until the new compositor says `READY`; then
sessiond has the greeter give the display back and end. The text console is
never shown in between.

The session compositor uses its control socket for a few requests:

| Request | Meaning |
| --- | --- |
| `UNLOCK style`, then the secret on the next line | The lock screen: check the session user's password, PIN or security key, as at login. The user is the session's, not a name the compositor sends. |
| `CANCEL` | Stop a security key attempt under way. |
| `ENROLLED` | The session user's PIN and security keys, without secrets (Settings > Users). |
| `ENROLL pin` / `ENROLL fido2 label`, then the current password and the new PIN or the key's PIN on the next lines | Set the PIN or register a security key for the session user. |
| `REMOVE pin` / `REMOVE fido2 id`, then the current password on the next line | Remove the PIN or a security key of the session user. |
| `LOGOUT` | End the session and hand the display to a new greeter. |
| `SERVICE name on\|off\|status` | A system service the Sharing page controls (sshd only; root or wheel). |

The socket pairs are created by sessiond when it starts each process. No
socket that another process could connect to exists.

### The lock screen

The lock screen is drawn by the user's own compositor, which runs as the user.
It sends what the user typed to sessiond with `UNLOCK`, so the compositor
never sees a password hash, a PIN hash or a key's stored public key. Settings
> Users sets the PIN and registers security keys through the compositor's
system extension, which passes the request to sessiond on the same control
socket.

## Related documents

- [Kernel, HAL and driver boundaries](kernel-and-hal.md)
- [Graphics and Wayland architecture](graphics-and-wayland.md): GPU capability handles, WSI, and `/bin/wayland` modes
- [Session management and graphical login](session-management.md): `sessiond`, greeter, and console fallback
- [Audio architecture and sound subsystem](audio.md): `audiod`, shared memory rings, and `libpulse`
- [Keiland on Linux](../../userland/desktop/LINUX.md) and
  [on FreeBSD](../../userland/desktop/README.freebsd.md): build and launch

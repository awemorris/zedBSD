mikuterm (Micro Unicode Terminal)
=================================

mikuterm is a terminal for Windows whose tabs are SSH connections.  It runs
from this folder: nothing is installed, nothing is written to the registry,
and it reads and writes only the files of this folder.  It needs Windows 10
version 1903 or later, on x64 or ARM64.


1. The folder
-------------

  mikuterm.exe           the program
  README.txt             this file
  config/mikuterm.conf   your settings (fonts, size, colours, the first host)
  config/state.conf      what the View menu and the settings chose at once
                         (mikuterm writes it)
  ssh/config             the hosts you connect to, in OpenSSH's ssh_config form
  ssh/id_ed25519         your private key (you put it here)
  ssh/known_hosts        the hosts' keys you accepted (mikuterm writes it)
  fonts/                 your own fonts
  share/fonts/           the fonts mikuterm brings
  share/licenses/        the licenses of mikuterm and of its fonts

Paths in this file and on mikuterm's screen are written with / between
folders, as in ssh/config; Windows takes / and \ alike, so either opens the
same file.

Copy the whole folder to move mikuterm to another place or another PC: your
settings, hosts, key and known hosts go with it.  Keep the folder where you
can write (not under "Program Files", and not inside the ZIP file itself), or
mikuterm cannot keep the hosts' keys, the View menu's choices and the
settings.


2. Connecting
-------------

Start mikuterm.exe.  A new tab (Shell > New Tab, Ctrl+Shift+T) shows the
hosts of ssh/config:

  MikμTerm 0.1.0

  Hosts:
   [1] me@server
   [2] backup
   [w] WSL
   [x] Settings

  >>> me@server

Up and Down choose a row, and Enter opens it.  A row's number (or w, or x)
chooses that row too.  The ">>>" line holds the chosen row; to connect to a
host that is not in the list, type it there instead (user@host:port, or a
name): the first character you type takes the row's place.  Backspace and
Ctrl+U edit the line, and Escape puts the chosen row back.  A name that
starts with a digit, w or x is typed after Ctrl+U (those keys choose rows).
A row shows user@host when ssh/config gives the host a User.

[w] WSL lists WSL's distributions (section 5).  [x] Settings opens the
settings in a tab of their own beside this one (section 3): the tab stays,
and you come back to it with the tab bar or F12.  Ctrl+C closes the tab.

A window has up to ten tabs.  These keys go between them:

  Ctrl+Alt+1 to 9     the tab of that place, counted from the left
  Ctrl+Alt+0          the tenth tab
  Ctrl+Alt+Page Up    the tab before (the last one after the first)
  Ctrl+Alt+Page Down  the tab after (the first one after the last)

Use the left Alt key for them.  The right Alt key is AltGr on the keyboards
that have one, and AltGr with a digit still types its character.

A tab is named "1 mikuterm", "2 mikuterm" and so on in the order the tabs
were opened, until the host gives it a title of its own.  When the tabs do
not fit in the title bar, it shows as many as fit between two arrows (< and
>, or the mouse wheel over the tabs) and a button (v) that lists the tabs
out of sight; a tab chosen by its keys is always brought into sight.

When ssh/config gives no User for the host, the tab asks "login as:".

The first time you connect to a host, mikuterm shows the host's key and asks
whether to trust it, as OpenSSH's ssh does:

  The authenticity of host 'server.example.org (192.0.2.10)' can't be established.
  ED25519 key fingerprint is SHA256:...
  This key is not known by any other names.
  Are you sure you want to continue connecting (yes/no/[fingerprint])?

Compare the fingerprint with the one the host's administrator gives you (on
the host: ssh-keygen -l -f /etc/ssh/ssh_host_ed25519_key.pub), then type yes,
or paste the fingerprint itself.  The key is added to ssh/known_hosts and you
are not asked again.

If a host later shows a key that is not the one ssh/known_hosts holds,
mikuterm warns "REMOTE HOST IDENTIFICATION HAS CHANGED!" and does not
connect.  When you know the host's key was replaced, delete the line the
warning names from ssh/known_hosts (in the settings' Keys pane, or with
Notepad) and connect again.

A tab closes when its connection ends normally (exit on the host).  When a
connection fails, the tab says why; Enter connects again and Ctrl+C closes
the tab.


3. The settings (F12)
---------------------

F12 (Shell > Settings...) opens the settings in a tab of their own, and F12
on that tab goes back to the tab you came from.  [x] Settings on a new tab's
screen opens them as well.  The sidebar has four panes (Ctrl+Down or
Ctrl+Page Down shows the next one, Ctrl+Up or Ctrl+Page Up the one before;
Tab moves between a pane's fields and buttons):

  Hosts       the hosts of ssh/config, one at a time: its name, address
              (HostName), port, user, command (RemoteCommand), identity
              files and port forwards (LocalForward, RemoteForward), and Add
              Host.  Only the lines you change are rewritten; your comments
              and the lines the form does not show stay as they are.  Save
              (Ctrl+S) checks the file as a connection reads it and refuses
              a line you changed that it cannot use (a problem the file had
              already does not stop it); it does not write over a file
              changed outside since it was read (Revert, Ctrl+R, reads it
              again).
              Open in Text Editor opens ssh/config in Windows' editor for
              it (Notepad when none is set).  Both take effect from the
              next connection.
  Appearance  the text's size, the theme and the ambiguous width take effect
              at once, as the View menu's do; the fonts and the grid when
              mikuterm starts again.
  Behavior    copy-on-select takes effect at once; TERM, the connect
              timeout and the port of WSL's sshd from the next connection;
              the host of the first tab when mikuterm starts again.
  Keys        makes an ed25519 key, shows a key's public key (Copy Public
              Key), and lists ssh/known_hosts, each line with a button that
              removes it (after a host's key was replaced).

What takes effect at once is kept in config/state.conf.  The rest is written
into config/mikuterm.conf in place of its line; your other lines and
comments stay as you wrote them.

A key from the Keys pane: Name (id_ed25519 unless you choose another),
Comment, and a passphrase typed twice (or none).  Make Key writes ssh/<name>
and ssh/<name>.pub in OpenSSH's form, as ssh-keygen -t ed25519 writes them;
with a passphrase the private key is encrypted as ssh-keygen encrypts it.  A
key that is there already is never written over: choose another name.  Only
your Windows account may open the new private key file.  Copy Public Key puts
the line of the .pub on the clipboard: add it to the host's
~/.ssh/authorized_keys.  A key of another name than id_ed25519 is used for a
host whose lines in ssh/config name it (IdentityFile ~/.ssh/<name>).

The settings tab follows the screen's scale, as the window's text does.


4. Your key and ssh/config
--------------------------

mikuterm uses an ed25519 key, with or without a passphrase.  Make one in the
settings' Keys pane (section 3), or with OpenSSH on any machine (Windows has
it too):

  ssh-keygen -t ed25519 -f id_ed25519

Put id_ed25519 into ssh/ and add the line of id_ed25519.pub to the host's
~/.ssh/authorized_keys.  When the host does not take a key, mikuterm asks
for the password.

A key with a passphrase (ssh-keygen's aes256-ctr and bcrypt) is asked for
its passphrase only when the host would take it:

  [x] Remember the passphrase until you sign out of Windows (Tab switches this)
  Enter passphrase for key 'ssh/id_ed25519':

What you type is not shown.  With the box checked (Tab switches it), a
passphrase that opens the key is kept in Windows' Credential Manager for your
Windows session: it stays in memory, is never written to the disk, and goes
when you sign out.  The next connection uses it without asking.  Session >
Forget Remembered Passphrases forgets every passphrase mikuterm kept.

ssh/config is written as OpenSSH's ~/.ssh/config:

  Host server
      HostName server.example.org
      User me

mikuterm reads Host, HostName, User, Port, IdentityFile,
StrictHostKeyChecking, UserKnownHostsFile, GlobalKnownHostsFile, BatchMode,
ConnectTimeout, ServerAliveInterval, ServerAliveCountMax, RequestTTY,
PasswordAuthentication, PubkeyAuthentication, LogLevel, ForwardX11,
ForwardX11Trusted and ForwardX11Timeout (see 10), LocalForward,
RemoteForward, GatewayPorts, ExitOnForwardFailure, ClearAllForwardings and
RemoteCommand (see 11).  RequestTTY and LogLevel are checked but change
nothing: a tab is always a terminal, and mikuterm shows no debug lines.
SessionType none is refused.  Match, Include, ProxyJump and the other
keywords it does not know are errors that name their
line, so that a setting that would not work is noticed.  Paths: ~/.ssh/ is
this folder's ssh/, ~/ is this folder, a name without a folder is in ssh/,
and a full path (C:/... or //server/share/...) is taken as it is.

ssh/known_hosts has one line a host, as OpenSSH writes it ("[host]:port" for
a port other than 22).  mikuterm does not write hashed names (HashKnownHosts
yes is an error) and cannot read them: if you copy a known_hosts whose lines
start with "|1|" (Ubuntu and Debian hash them), those hosts are asked about
again.  Lines with plain names are read as they are.


5. WSL
------

mikuterm connects to WSL through SSH, as to any other host: an sshd runs in
the distribution, and WSL 2 passes localhost's port on to it.

[w] WSL on a new tab's screen asks Windows (wsl.exe -l -q) for the
distributions and lists them; Up, Down or a number and Enter choose one, [b]
(or Escape) goes back.  mikuterm then starts the distribution
(wsl.exe -d <name> -e whoami, which also starts an sshd that systemd runs)
and connects to it:

  - when ssh/config has a Host named wsl-<name> (for example
    "Host wsl-Ubuntu-24.04"; the case does not matter), with that Host's
    HostName, Port, User and other settings;
  - otherwise to localhost on the port WslPort gives in config/mikuterm.conf
    (2222 unless you change it; the settings' Behavior pane has it too).

The user is the Host's User, or else the distribution's default user (the one
whoami answers with); when neither is known the tab asks "login as:".

All the distributions of WSL 2 share one network, so only one of them can
listen on a port.  For one distribution, WslPort is enough.  For more, give
each its own port and a Host:

  Host wsl-Ubuntu-24.04
      HostName localhost
      Port 2222
  Host wsl-Debian
      HostName localhost
      Port 2223

When Windows has no WSL, or no distribution, or wsl.exe does not answer, the
screen says so and stays on the hosts.

In the distribution (Ubuntu here):

  sudo apt install openssh-server
  sudo sed -i 's/^#\?Port .*/Port 2222/' /etc/ssh/sshd_config
  sudo service ssh start
  mkdir -p ~/.ssh && chmod 700 ~/.ssh
  cat id_ed25519.pub >> ~/.ssh/authorized_keys

(With systemd in WSL, "sudo systemctl enable --now ssh" starts it with WSL.
Ubuntu 24.04 starts sshd from ssh.socket: after changing the port, run
"sudo systemctl daemon-reload" and "sudo systemctl restart ssh.socket".)

To open a distribution in the first tab whenever mikuterm starts, give it a
Host (as above) and put "Connect wsl-<name>" into config/mikuterm.conf.


6. config/mikuterm.conf
-----------------------

One "Keyword value" a line, as ssh_config.  A line mikuterm cannot use is
named in the first tab and skipped.

  Font            the grid's font (keiland-mono.ttf)
  FontFallback    the font for what the grid's font has not (keiland-fallback.ttf)
  FontSize        the font's size in pixels at 96 dpi, 8 to 32 (16)
  Columns, Rows   the grid the window opens with (90, 28)
  Theme           dark, light or contrast (dark)
  AmbiguousWide   yes: characters of ambiguous width take two cells (no)
  Term            the TERM the host is told (xterm-256color)
  Connect         the host the first tab connects to (none: the list)
  CopyOnSelect    yes: what the pointer selects is copied at once (yes)
  ConnectTimeout  how many seconds a connection may take, 0 for no limit (15)
  WslPort         the port of the sshd in WSL, for a distribution without a
                  Host wsl-<name> in ssh/config (2222)
  X11Scale        the size of the remote X programs' windows: auto (the
                  main monitor's scale) or 100, 125, 150, 175, 200 ... 400
                  percent (auto)
  X11Render       auto or cpu: how the X windows are drawn (auto)
  X11PrimaryToClipboard  yes: what an X program selects (X's PRIMARY, which
                  xterm copies to) also goes to the Windows clipboard (no)

What the View menu and the settings choose at once (Text Size, Theme, Treat
Ambiguous-Width Characters as Wide, Alt+F11, copy-on-select) is kept in
config/state.conf and wins over the same settings here; delete state.conf to
go back to mikuterm.conf.  The settings' other choices are written here.


7. Passwords and the input method
---------------------------------

While mikuterm asks for a password, what you type is not shown and the input
method (IME) is off.  mikuterm cannot see when a program on the host (sudo,
su, passwd) asks for one: turn the input method off yourself with
Session > Disable Input Method (Ctrl+Shift+I), and on again after it.

The function keys F1 to F11 go to the host (htop, mc).  mikuterm's own keys
are F12 (the settings), Alt+F11 (Treat Ambiguous-Width Characters as Wide),
Alt+Enter (Fullscreen) and Ctrl+Alt with a digit, Page Up or Page Down (the
tabs, section 2).


8. Fonts
--------

Put a TrueType font (.ttf or .ttc with glyf outlines) into fonts/ and name it
in the settings' Appearance pane, or in config/mikuterm.conf:

  Font MyFont.ttf
  FontFallback MyCJKFont.ttc

A font in fonts/ is used before one of the same name in share/fonts/.  A
full path (C:/Windows/Fonts/consola.ttf, say) is taken as it is: one that
starts at a drive (C:/) or a server (//server/share/).  Anything else, such as
/Windows/..., \Windows\... or C:x.ttf, is a name in fonts/ or share/fonts/.


9. Licenses
-----------

Copyright (C) 2026 Awe Morris.  mikuterm.exe is under the zlib license
(share/licenses/zedBSD-LICENSE.txt).  The fonts in share/fonts are their
authors' works under their own licenses, listed in share/licenses/README.txt.


10. Remote X programs (X11 forwarding)
--------------------------------------

mikuterm can show the X11 programs of a host as windows on this PC.  Turn it
on for the host in ssh/config:

  Host server
      HostName server.example.org
      ForwardX11 yes

The host's sshd needs "X11Forwarding yes" in its sshd_config and the xauth
program installed (Debian and Ubuntu: apt install xauth).  Then a program
started in the tab (xeyes &, for example) opens its window here.  Each
connection has an X server of its own, run by mikuterm in a process of its
own (mikuterm.exe --x11-server, at a low integrity level where Windows
allows it); it ends with the connection, and if it fails, the tab says so
and the next program starts a new one.

Every X window's title starts with the host's name in brackets, [server].
A remote program can draw a window that looks like anything: do not type a
password or passphrase of this PC or of mikuterm into an X window.

The clipboard: unless ForwardX11Trusted is yes, a remote program can read the
Windows clipboard only just after you paste in one of its windows (the
middle button, Ctrl+V, Ctrl+Shift+V or Shift+Insert), and can set it only
just after you copy in one of its windows (a selection with the left button,
Ctrl+C, Ctrl+Shift+C or Ctrl+Insert).  With ForwardX11Trusted yes it may do
both at any time.  ForwardX11Timeout (for example 20m) refuses new X programs
of an untrusted connection after that time; unlike OpenSSH's, it is off
unless you set it.

When the shell ends while X programs still run, the connection stays open
until the last of them ends; close the tab to end them all.

The X server is new: it shows the classic X programs (xterm, xclock, xeyes,
xlogo).  Programs that need what it does not have yet (GTK and many Qt
programs need RENDER and XInput2) stop with an X error for now.  A window
whose program asks for no position may open at the top left of another
display; Win+Shift+Left or Right moves it.

The X server does not offer OpenGL (GLX): remote programs that draw with
OpenGL through the X server do not show here.

GTK 4 programs draw with OpenGL on the host by default, even over ssh (in
software there), and send each window whole as an image every time it
changes, which is slow over a network.  Set GSK_RENDERER=cairo on the host
to make them draw with Cairo, which sends far less:

  GSK_RENDERER=cairo gtk4-demo

or, for every program of the login, add this line to ~/.profile on the
host:

  export GSK_RENDERER=cairo

GTK programs use the host's D-Bus session.  When the login shares one with
a desktop on the host, a program may wait about half a minute for the
desktop's services before its window opens; started in a D-Bus session of
its own, it does not wait:

  dbus-run-session gtk3-demo


11. Port forwarding and RemoteCommand
-------------------------------------

A host's LocalForward and RemoteForward lines forward TCP ports, as OpenSSH's
ssh -L and -R do:

  Host db
      HostName server.example.org
      LocalForward 8080 127.0.0.1:80
      RemoteForward 9000 localhost:3000

LocalForward: mikuterm listens on this PC (on 127.0.0.1 and ::1 only, unless
GatewayPorts is yes or an address such as * is given before the port), and a
connection to 8080 goes to 127.0.0.1:80 as the server sees it.  RemoteForward:
the server listens on 9000, and a connection to it comes to localhost:3000 of
this PC.  Port 0 lets the server choose; the tab shows the port.  With
ExitOnForwardFailure yes, a forwarding that cannot be set up ends the
connection.  A port mikuterm listens on is its own: no other program can
take it meanwhile.  Listening on every address may make Windows Defender
Firewall ask about mikuterm.

The tab shows a line only when something is wrong, or for the port the
server chose for a RemoteForward 0: "bind [127.0.0.1]:8080: Address already
in use" (the port is taken, by another tab of the same host too), "Warning:
remote port forwarding failed for listen port 9000" (the server refused it),
"Allocated port 40123 for remote forward to localhost:3000".  When the
shell ends while forwarded connections are open, the connection stays until
they end; close the tab to end them.  DynamicForward (SOCKS) is not
supported.

RemoteCommand runs a command instead of the shell, on a terminal as the
shell would be, with OpenSSH's tokens (%h, %n, %k, %p, %r, %u, %d, %l, %L,
%j, %%); %C and %i are errors.

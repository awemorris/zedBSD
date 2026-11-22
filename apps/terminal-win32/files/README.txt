mikuterm (Micro Unicode Terminal)
=================================

A terminal with tabs whose tabs are SSH connections.  It runs from this
folder: nothing is installed, and it reads and writes only here.

  mikuterm.exe           the program
  config\mikuterm.conf   your settings (fonts, size, colours, the first host)
  config\state.conf      what the View menu chose (mikuterm writes it)
  ssh\config             the hosts, in OpenSSH's ssh_config form
  ssh\id_ed25519         your key (an unencrypted ssh-ed25519 key)
  ssh\known_hosts        the hosts' keys, added as you accept them
  fonts\                 your own fonts
  share\                 the program's fonts and the licenses

A new tab lists the hosts of ssh\config and asks "Host:" (a name, its number,
or user@host:port).  A tab closes when its connection ends normally; when it
fails, the tab says why and Enter connects again.

What you select with the pointer is copied to the clipboard (CopyOnSelect).

Copyright (C) 2026 Awe Morris.  The program is under the zlib license
(share\licenses\zedBSD-LICENSE.txt); the fonts are under their own licenses
(share\licenses\README.txt).

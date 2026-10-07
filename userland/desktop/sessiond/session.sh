#!/bin/sh
# The graphical session sessiond starts after a login, as the user, with HOME, USER, LOGNAME, PATH, SHELL and
# XDG_RUNTIME_DIR (the user's own directory, /run/user/UID) set.  Installed as /etc/keiland/session.  The
# session lasts as long as the compositor does: App Home's Log Out ends it, and sessiond shows the greeter again.
# sessiond passes --control-fd=3 (its socket; the compositor says READY on it before it takes the display from the
# greeter), which goes to the compositor with the other arguments.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# The usual folders, which Files shows in its sidebar and on its Home page (like xdg-user-dirs).
for folder in Desktop Documents Downloads Pictures Music Movies; do
	mkdir -p "$HOME/$folder"
done

# The compositor, with the wallpaper when the image has one; its socket in the runtime directory.
picture=
[ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
exec /bin/wayland --session --glass --socket="$XDG_RUNTIME_DIR/wayland-0" $picture "$@"

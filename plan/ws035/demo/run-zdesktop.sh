#!/bin/sh
# WS035 p069: the compositor on the machine's own display, started again if it ever ends (its deadline is
# a day).  Its log is kept in /var/log/zwl.log.
export XDG_RUNTIME_DIR=/tmp
sleep 3
while :; do
	rm -f /tmp/wayland-0
	picture=
	[ -f /usr/share/zdesktop/wallpaper.ppm ] && picture=--wallpaper=/usr/share/zdesktop/wallpaper.ppm
	/bin/zwl --socket=/tmp/wayland-0 --timeout=86400 --glass $picture >> /var/log/zwl.log 2>&1
	sleep 2
done

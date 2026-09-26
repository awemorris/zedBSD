#!/bin/sh
# WS035 p066: the compositor, its output kept on the disk for plan/ws031/tests/ufs-cat.py.
exec /bin/zwl --socket=/tmp/wayland-0 --width=1920 --height=1080 --timeout=300 --glass --wallpaper=/usr/share/zdesktop/wallpaper.ppm --log-frames > /var/log/zwl.log 2>&1

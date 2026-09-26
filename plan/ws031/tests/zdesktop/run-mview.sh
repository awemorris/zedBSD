#!/bin/sh
# WS035 p066: the model viewer as a window, its output kept on the disk.
/bin/mview --display=/tmp/wayland-0 --windowed --size=800x560 --token=zd1 --timeout-s=250 > /var/log/mview.log 2>&1
sync

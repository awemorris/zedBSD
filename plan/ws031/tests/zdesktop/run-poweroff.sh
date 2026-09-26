#!/bin/sh
# WS035 p066: the kernel's messages kept on the disk, then a clean power-off.
dmesg > /var/log/dmesg.log 2>&1
sync
exec /sbin/poweroff

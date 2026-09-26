#!/bin/sh
# WS031 p050: a Vulkan window killed while it draws, so the executor's session closes with the
# client's objects still alive; the compositor must keep drawing and the viewer after it must work.
/bin/timeout -s KILL 6 /bin/wltest --display=/tmp/wayland-0 --windowed --size=800x560 --color=c04040 --frames=3600 --delay-ms=16 --token=wk > /var/log/wlkill.log 2>&1
echo "WLKILL status=$?" >> /var/log/wlkill.log
sync

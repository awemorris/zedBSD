#!/bin/sh
# WS035 p068: the terminal in place of the model viewer (ZDESKTOP_APP=terminal), its output kept on the disk.
# The logs and the kernel's messages are written out every two seconds, so that they survive a run the
# harness ends by stopping QEMU.
(while :; do dmesg > /var/log/dmesg.log 2>&1; sync; sleep 2; done) &
/bin/zdesktop-terminal --display=/tmp/wayland-0 --columns=78 --rows=24 --token=zd1 --timeout-s=250 --command='echo zdesktop-terminal on i915; uname -a; ls /; exec /bin/sh' > /var/log/mview.log 2>&1
sync

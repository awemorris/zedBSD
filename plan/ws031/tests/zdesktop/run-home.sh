#!/bin/sh
# WS035 p069: nothing in the viewer's place; App Home starts the applications.  The logs and the kernel's
# messages are written out every two seconds for 200 seconds, then the guest goes on to power off.
i=0
while [ $i -lt 100 ]; do dmesg > /var/log/dmesg.log 2>&1; sync; sleep 2; i=$((i+1)); done

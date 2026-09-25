#!/bin/sh
# Checks that networkd tells a watcher when the network moves.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The daemon is asked to watch, the interface is taken up and down, and the
# frames the watch printed are counted.  One frame arrives at once (the
# state as it stands) and more arrive as the interface moves; a daemon that
# does not notify prints exactly one and then nothing.
#
# The result goes to the console as one line, which is what the screen shows
# and what the test reads back.

output=/tmp/networkd-watch.txt
net watch > "$output" 2>&1 &
watcher=$!
sleep 2
before=$(grep -c '^' "$output")
net up eth0 > /dev/null 2>&1
sleep 2
net down eth0 > /dev/null 2>&1
sleep 2
after=$(grep -c '^' "$output")
kill "$watcher" 2>/dev/null
sleep 1

# The watch must have said something at once, and more once things moved.
if [ "$before" -ge 1 ] && [ "$after" -gt "$before" ]; then
	echo "NETWORKD-WATCH PASS first=$before later=$after"
else
	echo "NETWORKD-WATCH FAIL first=$before later=$after"
fi

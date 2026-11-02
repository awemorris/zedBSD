#!/bin/sh
# From the host (the top of the tree): saves the Pi 5's USB HID report ring with ring-save.sh on the board, fetches it
# into build/bug412-ring/STAMP and decodes it (ring-decode.py; extra arguments go to it, such as --at EPOCH).
# usage: PI5=10.0.30.7 sh plan/bugs/BUG-412/ring-collect.sh [ring-decode.py options]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
PI5=${PI5:-10.0.30.7}
PW=${PI5_PASSWORD:-user}
OUT=${OUT:-build/bug412-ring/$(date +%Y%m%d-%H%M%S)}
SO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o PubkeyAuthentication=no"
b() {
	timeout 120 sshpass -p "$PW" ssh $SO "user@$PI5" "$@"
}
mkdir -p "$OUT" || exit 1
b 'cat > /tmp/ring-save.sh' < plan/bugs/BUG-412/ring-save.sh || { echo "FAIL $PI5 unreachable"; exit 1; }
line=$(b "echo $PW | sudo -S sh /tmp/ring-save.sh 2> /dev/null" | grep -E '^(SAVED|FAIL)')
echo "board: $line"
dir=$(echo "$line" | sed -n 's/^SAVED \([^ ]*\) .*/\1/p')
[ -n "$dir" ] || exit 1
for f in ring.bin session.log dmesg.txt info.txt; do
	b "echo $PW | sudo -S cat $dir/$f 2> /dev/null" > "$OUT/$f" || { echo "FAIL fetching $f"; exit 1; }
done
chmod 600 "$OUT"/*
sync
echo "fetched $OUT ($(wc -c < "$OUT/ring.bin") bytes of ring)"
python3 -I plan/bugs/BUG-412/ring-decode.py "$OUT" "$@"

#!/bin/sh
# ws334-p027: the browser's window on the zedBSD amd64 Venus guest, in one run (start, run, collect, summary).
# The image is plan/ws334/tests/config-amd64-p027-guest.mk's (build it first, with the storage page):
#   sh plan/tools/guest/test-image.sh plan/ws334/tests/config-amd64-p027-guest.mk build/p027g/img \
#       --file /usr/share/keiland/wallpaper.png=userland/desktop/wallpapers/Birch-Lake.png \
#       --file /usr/share/p027-tests/storage.html=plan/ws334/tests/p027-storage.html
# The compositor runs with --testing and --log-frames (its SHM_COPY lines name the clients that draw into shared
# memory) at 1280x800; the pictures are keiland-shot's.  Steps:
#  1. gpu-1: browser on the storage page (the Vulkan way): the first load writes localStorage.
#  2. gpu-2: browser again: the value is read back after the restart of the browser.
#  3. cpu-1: browser --no-gpu (the CPU way): the value again, the page in shared memory.
#  4. files, terminal and settings open a window each and are still running after 6 seconds.
#  5. The guest powers off; the guest boots again from the disk it wrote, and gpu-3 reads the value again.
# The page's title carries what it found ("p027 kept-27 loads=N"), which the browser's NAVIGATE line repeats.
#
#   sh plan/ws334/tests/p027-guest.sh [OUTDIR]      (default build/p027g/run; summary in OUTDIR/summary.txt)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
root=$(pwd)
out=${1:-build/p027g/run}
image=${P027_IMAGE:-build/p027g/img/hdd-image.img}
GUEST_RUNTIME=${GUEST_RUNTIME:-$root/build/p027g/runtime}
VENUS_RENDERER=${VENUS_RENDERER:-/home/awe/zedBSD-claude1/build/ws035-sq-venus/install}
export GUEST_RUNTIME VENUS_RENDERER
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"
status=0
page=file:///usr/share/p027-tests/storage.html
say() { printf '%s\n' "$*" | tee -a "$summary"; }
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }

# Boots an image and waits for SSH; the compositor then runs in the guest.
boot() {
	timeout 120 sh plan/ws035/tests/zdesktop-guest.sh start "$1" > "$out/start-$2.txt" 2>&1 || { say "boot $2: start FAILED"; return 1; }
	timeout 320 python3 plan/tools/guest/guest.py wait --timeout 300 >> "$out/start-$2.txt" 2>&1 || { say "boot $2: no SSH"; return 1; }
	say "boot $2: ssh up"
	guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0 /tmp/keiland-shot.sock
/bin/wayland --testing --timeout=1800 --width=1280 --height=800 --log-frames --wallpaper=/usr/share/keiland/wallpaper.png > /tmp/kwl.log 2>&1 </dev/null & sleep 5; echo started' > /dev/null
	guest 'ls /tmp/wayland-0 /tmp/keiland-shot.sock' > "$out/compositor-$2.txt"
	if grep -q 'No such' "$out/compositor-$2.txt"; then
		say "boot $2: compositor sockets MISSING"
		return 1
	fi
	say "boot $2: compositor up"
}

# The last client the compositor mapped a surface of.
last_client() { guest "grep 'KWL MAP client=' /tmp/kwl.log | tail -1" | sed -n 's/.*client=\([0-9]*\) .*/\1/p'; }

# Takes the screen into OUTDIR/NAME.png.
shot() {
	guest "/bin/keiland-shot --socket /tmp/keiland-shot.sock /tmp/$1.png" > "$out/$1.shot.txt"
	timeout 60 python3 plan/tools/guest/guest.py get "/tmp/$1.png" "$out/$1.png" > /dev/null 2>&1
	if [ -s "$out/$1.png" ]; then say "$1: picture $out/$1.png ($(cat "$out/$1.shot.txt" | tail -1))"; else say "$1: picture MISSING ($(tail -1 "$out/$1.shot.txt"))"; status=1; fi
}

# Runs the browser on the storage page (NAME, arguments, the loads expected), takes the picture and stops it.
browser_run() {
	name=$1 arguments=$2 loads=$3 value=$4
	before=$(last_client)
	guest "export XDG_RUNTIME_DIR=/tmp; /bin/browser --width=900 --height=600 $arguments $page > /tmp/$name.log 2>&1 </dev/null & echo \$! > /tmp/$name.pid; sleep 1; echo started" > /dev/null
	tries=0
	while [ $tries -lt 40 ]; do
		ready=$(guest "grep -c 'ZBROWSER READY' /tmp/$name.log" | tail -1)
		[ "${ready:-0}" -gt 0 ] 2>/dev/null && break
		sleep 1
		tries=$((tries + 1))
	done
	sleep 4
	client=$(last_client)
	shot "$name"
	guest "cat /tmp/$name.log" > "$out/$name.log"
	navigate=$(grep 'ZBROWSER NAVIGATE' "$out/$name.log" | tail -1)
	fallback=$(grep -c 'cannot draw with the GPU' "$out/$name.log")
	copies=$(guest "grep -c 'KWL SHM_COPY client=$client ' /tmp/kwl.log" | tail -1)
	surfaces=$(guest "grep 'KWL SHM_COPY client=$client ' /tmp/kwl.log | sed 's/.* surface=\\([0-9]*\\) .*rows=\\([0-9]*-[0-9]*\\).*/s\\1:\\2/' | sort | uniq -c | sort -rn | head -4" | tr -s ' \n' ' ')
	say "$name: ready=${ready:-0} client=$client (before $before) gpu-fallback-lines=$fallback shm-copies=$copies"
	say "$name: shm copies by surface:rows (count first): $surfaces"
	say "$name: $navigate"
	if printf '%s\n' "$navigate" | grep -q "title=p027 $value loads=$loads"; then say "$name: title ok (p027 $value loads=$loads)"; else say "$name: title MISSING (want p027 $value loads=$loads)"; status=1; fi
	guest "kill \$(cat /tmp/$name.pid); sleep 2; kill -0 \$(cat /tmp/$name.pid) 2>/dev/null && echo still-running || echo stopped" | tail -1 > "$out/$name.stop.txt"
	say "$name: $(cat "$out/$name.stop.txt")"
}

# Opens an application (NAME, command), checks it maps a window and still runs after 6 seconds, takes the picture.
app_run() {
	name=$1 command=$2
	before=$(last_client)
	guest "export XDG_RUNTIME_DIR=/tmp; $command > /tmp/$name.log 2>&1 </dev/null & echo \$! > /tmp/$name.pid; sleep 6; echo started" > /dev/null
	client=$(last_client)
	alive=$(guest "kill -0 \$(cat /tmp/$name.pid) 2>/dev/null && echo running || echo exited" | tail -1)
	shot "$name"
	guest "cat /tmp/$name.log" > "$out/$name.log"
	if [ -n "$client" ] && [ "$client" != "$before" ] && [ "$alive" = running ]; then
		say "$name: window ok (client $client, before $before, $alive)"
	else
		say "$name: window MISSING (client $client, before $before, $alive)"
		status=1
	fi
	guest "kill \$(cat /tmp/$name.pid); sleep 2" > /dev/null
}

[ -s "$image" ] || { say "no image $image"; exit 1; }
boot "$image" 1 || { timeout 60 sh plan/ws035/tests/zdesktop-guest.sh stop > /dev/null 2>&1; sync; exit 1; }
guest 'uname -a; echo HOME=$HOME XDG_DATA_HOME=${XDG_DATA_HOME:-unset}; ls -l /bin/browser /bin/files /bin/terminal /bin/settings /bin/keiland-shot' > "$out/guest-info.txt"
browser_run gpu-1 "" 1 none
browser_run gpu-2 "" 2 kept-27
browser_run cpu-1 "--no-gpu" 3 kept-27
guest 'ls -laR $HOME/.local/share/keiland 2>&1 | head -30' > "$out/storage-dir.txt"
say "storage dir: $(wc -l < "$out/storage-dir.txt") lines ($out/storage-dir.txt)"
app_run files /bin/files
app_run terminal /bin/terminal
app_run settings /bin/settings
guest 'cat /tmp/kwl.log' > "$out/kwl-1.log"

# The guest powers off, and boots again from the disk it wrote.
guest 'sync; sync; (sleep 1; poweroff) > /dev/null 2>&1 < /dev/null & echo off' > /dev/null
pid=$(sed -n 's/.*"pid": \([0-9]*\).*/\1/p' "$GUEST_RUNTIME/session.json" 2>/dev/null)
tries=0
while [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && [ $tries -lt 60 ]; do sleep 1; tries=$((tries + 1)); done
if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then say "poweroff: the emulator still ran after 60 s; stopped"; fi
timeout 60 sh plan/ws035/tests/zdesktop-guest.sh stop > /dev/null 2>&1
sync
cp "$GUEST_RUNTIME/disk.img" build/p027g/disk-after.img && sync
say "poweroff: disk kept as build/p027g/disk-after.img"
if boot build/p027g/disk-after.img 2; then
	browser_run gpu-3 "" 4 kept-27
	guest 'cat /tmp/kwl.log' > "$out/kwl-2.log"
else
	status=1
fi
timeout 60 sh plan/ws035/tests/zdesktop-guest.sh stop > /dev/null 2>&1
sync
if [ $status = 0 ]; then say "p027-guest: PASS"; else say "p027-guest: FAIL"; fi
exit $status

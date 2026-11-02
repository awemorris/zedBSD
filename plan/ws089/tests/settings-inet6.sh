#!/bin/sh
# BUG-413: Settings' Ethernet page shows the wired interface's IPv6 addresses and IPv6 router, on the Venus guest of
# the Settings image (build-settings-image.sh) with the real networkd.  QEMU's user network sends Router
# Advertisements (fec0::/64, router fe80::2), from which networkd gives the guest's own wired interface a global
# address by SLAAC.
#  1. The guest's wired interface has a global IPv6 address and an IPv6 default route (ifconfig, route -6).
#  2. Settings (zdesktop --glass and Settings as root) on the Ethernet page logs the interface with its IPv6
#     (NETWORK wired-link ... inet6=N first6=ADDRESS/LENGTH router6=ROUTER), the first address one ifconfig lists
#     and global, the router the one route -6 lists; the page is drawn (ethernet-inet6.png, and scrolled to the IP
#     card, ethernet-inet6-card.png).
# PASS: every "ok" line and the last line settings-inet6: PASS.
#   GUEST_RUNTIME=... plan/ws089/tests/settings-guest.sh start IMAGE   (the guest must be up)
#   GUEST_RUNTIME=... plan/ws089/tests/settings-inet6.sh [OUTDIR]      (default build/ws089-shots/inet6)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws089-run}"
export GUEST_RUNTIME
out=${1:-build/ws089-shots/inet6}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[s]ettings" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland( |$)|[s]ettings" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
start_desktop='export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass $picture > /tmp/zdesktop.log 2>&1 </dev/null & for w in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do grep -q KWL.READY /tmp/zdesktop.log 2>/dev/null && break; sleep 0.5; done; sleep 1; echo started'
status=0
. plan/ws089/tests/settings-wait.sh
pass() { echo "$1: ok"; }
fail() { echo "$1: FAILED"; status=1; }
shot() {
	pointer move 1270 790 sleep 300
	check "$out/$1" >/dev/null
	echo "shot: $out/$1"
}

# 1. The guest's IPv6: a wired interface with a global address (SLAAC can take a few seconds after the start).
wait_guest
guest "$stop_all" >/dev/null
interface=
tries=0
while [ $tries -lt 30 ]; do
	guest 'ifconfig -a; echo ROUTES; route -6' > "$out/ifconfig.txt"
	interface=$(awk '/^[a-z][a-z0-9]*:/ { name = $1; sub(":", "", name) } /^ROUTES/ { exit } /inet6 / && $2 !~ /^fe80:/ && $2 != "::1" && name !~ /^lo/ { print name; exit }' "$out/ifconfig.txt")
	[ -n "$interface" ] && grep -q '^default ' "$out/ifconfig.txt" && break
	tries=$((tries + 1))
	sleep 2
done
echo "interface: ${interface:-none}"
[ -n "$interface" ] && pass "a wired interface with a global IPv6 address" || fail "a wired interface with a global IPv6 address"
global=$(awk -v name="$interface" '/^[a-z][a-z0-9]*:/ { now = $1; sub(":", "", now) } /^ROUTES/ { exit } now == name && /inet6 / && $2 !~ /^fe80:/ { print $2 "/" $4; exit }' "$out/ifconfig.txt")
router=$(awk -v name="$interface" '/^ROUTES/ { routes = 1; next } routes && $1 == "default" && $NF == name { print $2; exit }' "$out/ifconfig.txt")
echo "ifconfig: $global, route -6: ${router:-none}"
[ -n "$router" ] && pass "an IPv6 default route" || fail "an IPv6 default route"

# 2. Settings' Ethernet page.
guest "$start_desktop" >/dev/null
wait_desktop
guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/settings --timeout-s=600 ethernet > /tmp/s.log 2>&1 </dev/null & sleep 6; echo started" >/dev/null
find_window
line=
tries=0
while [ $tries -lt 15 ]; do
	line=$(guest "grep -a 'NETWORK wired-link name=$interface ' /tmp/s.log | tail -1" | tail -1)
	case $line in *"inet6=0 "*|"") ;; *) break ;; esac
	tries=$((tries + 1))
	sleep 1
done
echo "settings: $line"
first6=$(echo "$line" | sed -n 's/.* first6=\([^ ]*\).*/\1/p')
router6=$(echo "$line" | sed -n 's/.* router6=\([^ ]*\).*/\1/p')
case $line in *" inet6=0 "*|"") fail "Settings has the IPv6 addresses" ;; *) pass "Settings has the IPv6 addresses" ;; esac
[ -n "$global" ] && [ "$first6" = "$global" ] && pass "the first is the global one ifconfig lists ($first6)" || fail "the first is the global one ifconfig lists ($first6 / $global)"
[ -n "$router" ] && [ "$router6" = "$router" ] && pass "the IPv6 router is route -6's ($router6)" || fail "the IPv6 router is route -6's ($router6 / $router)"
shot ethernet-inet6.png
pointer move $((wx + 700)) $((wy + 400)) sleep 200 wheel-down wheel-down wheel-down wheel-down wheel-down wheel-down sleep 1500
shot ethernet-inet6-card.png
guest 'grep -a "ZSETTINGS NETWORK" /tmp/s.log' > "$out/settings.log"
guest "$stop_all" >/dev/null
[ $status = 0 ] && echo "settings-inet6: PASS" || echo "settings-inet6: FAIL"
exit $status

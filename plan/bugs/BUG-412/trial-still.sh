#!/bin/sh
# One click variant, N times, alternating between two Settings rows; prints how many changed the page.
# usage (on the board, root, aat-input at /tmp/aat-input, Settings open at x=370 y=188): trial-still.sh NAME COUNT IDLE_MS VARIANT
#   VARIANT: abs | rel | relmove | absmove | relhold
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
A=/tmp/aat-input
L=/run/user/1000/session.log
name=$1; count=$2; idle=$3; variant=$4
ok=0; i=0
while [ $i -lt $count ]; do
	if [ $((i % 2)) = 0 ]; then y=290; want=bluetooth; else y=512; want=sound; fi
	$A move-to 494 $y >/dev/null
	case $variant in
	rel*) $A sleep 100 >/dev/null; $A move 3 0 >/dev/null; $A sleep 30 >/dev/null; $A move -3 0 >/dev/null ;;
	esac
	$A sleep $idle >/dev/null
	n=$(wc -l < $L)
	case $variant in
	abs) $A down left >/dev/null; $A sleep 50 >/dev/null; $A up left >/dev/null ;;
	absmove) $A down left >/dev/null; $A sleep 25 >/dev/null; $A move-to 496 $y >/dev/null; $A sleep 25 >/dev/null; $A up left >/dev/null ;;
	rel) $A mouse-down left >/dev/null; $A sleep 50 >/dev/null; $A mouse-up left >/dev/null ;;
	relmove) $A mouse-down left >/dev/null; $A sleep 25 >/dev/null; $A move 2 0 >/dev/null; $A sleep 25 >/dev/null; $A mouse-up left >/dev/null ;;
	relhold) $A mouse-down left >/dev/null; $A sleep 1300 >/dev/null; $A mouse-up left >/dev/null ;;
	esac
	sleep 0.7
	got=$(tail -n +$((n+1)) $L | grep -o -E 'ZSETTINGS PAGE [a-z-]+' | tail -1)
	press=$(tail -n +$((n+1)) $L | grep -c 'KWL PRESS')
	if [ "$got" = "ZSETTINGS PAGE $want" ]; then ok=$((ok + 1)); r=ok; else r="MISS($got)"; fi
	echo "$name $i $want $r press_lines=$press"
	i=$((i + 1))
done
echo "SUMMARY $name variant=$variant idle=$idle ok=$ok/$count"

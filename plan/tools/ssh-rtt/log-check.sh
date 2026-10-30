#!/bin/sh
# Run in the guest by qemu-ssh-rtt.sh (LOGCHECK=1): whether syslog loses lines, and what a stalled daemon costs a sender.
#  1. 300 lines from one logger in a burst, then 50 loggers of one line each: all 350 must reach /var/log/messages.
#  2. syslogd stopped (SIGSTOP) while one logger sends 30 lines: the sender must finish in a few seconds (each line
#     waits for room at most 100 ms), and once syslogd runs again the logger's next line must be followed by the count
#     of the lines it dropped.  The first lines (those the socket's queue held) and the next line must be in the log.
# Prints "log-check: PASS" or "log-check: FAIL" with the counts.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
log=/var/log/messages
tag=lc$$
status=0
i=0
while [ $i -lt 300 ]; do
	echo "burst $i"
	i=$((i + 1))
done | logger -t "$tag"
i=0
while [ $i -lt 50 ]; do
	logger -t "$tag" "single $i"
	i=$((i + 1))
done
# On a slow disk the writer is behind (a write waits for the flush), so the lines are waited for, up to a minute.
waited=0
while :; do
	burst=$(grep -c "$tag.*: burst " $log)
	single=$(grep -c "$tag.*: single " $log)
	[ "$burst" = 300 ] && [ "$single" = 50 ] && break
	[ $waited -ge 60 ] && break
	sleep 1
	waited=$((waited + 1))
done
echo "log-check: burst $burst/300 single $single/50 (in the log ${waited} s after the last was sent)"
[ "$burst" = 300 ] && [ "$single" = 50 ] || status=1

pid=$(ps -e | awk '$4 == "/sbin/syslogd" { print $1 }')
[ -n "$pid" ] || { echo "log-check: no syslogd"; echo "log-check: FAIL"; exit 1; }
kill -STOP "$pid"
begin=$(date +%s)
( i=0; while [ $i -lt 30 ]; do echo "stalled $i"; i=$((i + 1)); done; sleep 6; echo "resumed" ) | logger -t "${tag}s" &
sender=$!
sleep 4
sent=$(date +%s)
kill -CONT "$pid"
wait $sender
end=$(date +%s)
waited=0
while :; do
	stalled=$(grep -c "${tag}s.*: stalled " $log)
	resumed=$(grep -c "${tag}s.*: resumed" $log)
	note=$(grep "messages were dropped because the log daemon was behind" $log | tail -1)
	[ "$resumed" = 1 ] && [ -n "$note" ] && break
	[ $waited -ge 60 ] && break
	sleep 1
	waited=$((waited + 1))
done
echo "log-check: syslogd stopped: ${stalled}/30 stalled lines kept, resumed $resumed, the sender's 30 lines took under $((sent - begin)) s, all $((end - begin)) s"
echo "log-check: note: ${note:-none}"
dropped=$(echo "$note" | sed -n 's/.*syslog: \([0-9]*\) messages were dropped.*/\1/p')
[ "$resumed" = 1 ] || status=1
[ -n "$dropped" ] && [ $((stalled + dropped)) = 30 ] || status=1
[ $status = 0 ] && echo "log-check: PASS" || echo "log-check: FAIL"
exit $status

#!/bin/sh
# In the guest: runs /root/flush-{static,dynamic,driver} with "exit" and "return", standard output on a file, a pipe
# and a pseudo-terminal (/root/ptyrun), and writes one line per case to /tmp/bug432/result.txt:
#   CASE PASS|FAIL status=N bytes=N
# PASS is the whole output ("bug432 MODE line\ntail-handler") and the status (3 for exit, 4 for return).
# Then /root/tls-check-static with its output on a file: "tls-check-static PASS" when its last line is there.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
d=/tmp/bug432
mkdir -p $d
result=$d/result.txt
: > $result
for program in static dynamic driver; do
	for mode in exit return; do
		case $mode in exit) want=3 ;; *) want=4 ;; esac
		printf 'bug432 %s line\ntail-handler' $mode > $d/expect.$mode
		base=$d/$program-$mode
		/root/flush-$program $mode > $base-file.out 2> /dev/null
		echo "status $?" > $base-file.out.rc
		{ /root/flush-$program $mode 2> /dev/null; echo "status $?" > $base-pipe.out.rc; } | cat > $base-pipe.out
		/root/ptyrun $base-tty.out /root/flush-$program $mode 2> /dev/null
		for kind in file pipe tty; do
			out=$base-$kind.out
			status=$(cat $out.rc 2> /dev/null)
			bytes=$(wc -c < $out | tr -d ' ')
			if cmp -s $d/expect.$mode $out && [ "$status" = "status $want" ]; then
				verdict=PASS
			else
				verdict=FAIL
			fi
			echo "$program-$mode-$kind $verdict ${status:-nostatus} bytes=$bytes" >> $result
		done
	done
done
/root/tls-check-static > $d/tls-check-static.out 2>&1
status=$?
if grep -q '^TLS-CHECK:PASS' $d/tls-check-static.out && [ $status = 0 ]; then verdict=PASS; else verdict=FAIL; fi
echo "tls-check-static $verdict status $status bytes=$(wc -c < $d/tls-check-static.out | tr -d ' ')" >> $result
echo "done $(grep -c PASS $result) pass $(grep -c FAIL $result) fail" >> $result

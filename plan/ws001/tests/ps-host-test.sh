#!/bin/sh
# ws001-p044: ps (userland/base/ps/main.c) on the host against a fake /dev/system (ps-host-fake.c: init, the
# caller's shell and its sleep in session 100, a root daemon of more than a day, a kernel process).  Each case is
# the arguments, the expected status and the expected output; the last line is "ps-host: PASS" or "ps-host: FAIL".
#   sh plan/ws001/tests/ps-host-test.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws001-p044}
mkdir -p "$out"
rename="-Dopen=ps_fake_open -Dclose=ps_fake_close -Dioctl=ps_fake_ioctl -Dgeteuid=ps_fake_geteuid -Dgetsid=ps_fake_getsid"
cc -std=gnu11 -O0 -U_FORTIFY_SOURCE -Wall -Wextra -Werror -Iinclude $rename -c userland/base/ps/main.c -o "$out/ps.o" || exit 1
cc -std=gnu11 -O0 -Wall -Wextra -Werror -Iinclude -c plan/ws001/tests/ps-host-fake.c -o "$out/fake.o" || exit 1
cc -o "$out/ps" "$out/ps.o" "$out/fake.o" || exit 1
status=0
# case NAME STATUS EXPECTED ARGS...
case_() {
	name=$1 want_status=$2 want=$3
	shift 3
	got=$("$out/ps" "$@" 2>/dev/null)
	got_status=$?
	if [ "$got_status" = "$want_status" ] && [ "$got" = "$want" ]; then
		echo "ok: $name"
	else
		echo "FAIL: $name (status $got_status, want $want_status)"
		printf '%s\n--- want\n%s\n---\n' "$got" "$want"
		status=1
	fi
}
user0=$(id -nu 0)
case_ default 0 "PID TTY     TIME CMD
100 tty 00:00:02 -sh
101 tty 00:01:30 sleep"
case_ all-args 0 "PID COMMAND
  1 /sbin/init
100 -sh
101 sleep 60 abc, def
102 /sbin/daemond
103 kernel" -A -o pid,args
case_ no-headers 0 "101 sleep" -o pid= -o comm= -p 101
case_ header-to-end 0 "PID COMMAND LINE, WITH COMMA
101 sleep 60 abc, def" -o 'pid,args=COMMAND LINE, WITH COMMA' -p 101
case_ blanks 0 "PPID PID
 100 101" -o 'ppid pid' -p 101
case_ header-blanks 0 "  PID ppid
       101" -o 'pid=  PID ppid' -p 101
case_ full 0 "UID  PID PPID TTY     TIME CMD
4242 101  100 tty 00:01:30 sleep 60 abc, def" -f -p 101
case_ user-name 0 "PID USER
  1 $user0
102 $user0
103 $user0" -u "$user0" -o pid,user
case_ non-leaders 0 "PID
101
103" -d -o pid
case_ terminals 0 "PID
101" -a -o pid
case_ union 0 "PID
  1
101" -p 1 -g 101 -o pid
case_ day 0 "      TIME
1-01:00:00" -p 102 -o time
case_ unknown-cpu 0 "PID %CPU ELAPSED
  1    -       -" -p 1 -o pid,pcpu,etime
case_ unknown-field 1 "" -o pid,nosuch
case_ operand 1 "" 101
[ $status -eq 0 ] && echo "ps-host: PASS" || echo "ps-host: FAIL"
exit $status

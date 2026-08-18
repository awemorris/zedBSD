#!/usr/bin/env bash
# Exercise the ring-3 /bin/sh debug builtins against a writable FAT16 image.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

arch="${1:?usage: $0 pcat|pc98}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
build="$repo/build/$arch"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-sh-builtins-$arch.XXXXXX")"
keyboard_pid=
cleanup()
{
	if test -n "$keyboard_pid"; then
		kill "$keyboard_pid" 2>/dev/null || true
	fi
	rm -rf "$work"
}
trap cleanup EXIT
offset=$((2048 * 512))

case "$arch" in
pcat) build_platform=i386 ;;
pc98) build_platform=pc98 ;;
*) echo "unsupported platform: $arch" >&2; exit 2 ;;
esac

"$repo/build.sh" bios-hdd-image "$build_platform"
cp --reflink=auto "$build/bios-hdd-image.img" "$work/test.img"
printf '%s\n' 'zedBSD shell builtin copy fixture' >"$work/source.txt"
printf '%s\n' \
	'echo SHELL_ARGC_$#' \
	'echo SHELL_ARG1_$1' \
	'echo SHELL_ARG2_$2' \
	'shift' \
	'echo SHELL_SHIFT_$1' >"$work/argtest"
printf '%s\n' 'echo SHELL_DOT_PASS' >"$work/sourced"
printf '%s\n' 'SHELL_READ_PASS' >"$work/rdinput"
printf '%s\n' 'TEXT_XARGS_PASS' >"$work/xarginput"
printf '%s\n' \
	'pwd' \
	'ls /' \
	'ls -l /bin' \
	'/bin/ls -la /bin' \
	'echo EXTERNAL_LS_PASS' \
	'/bin/file /bin/sh /source.txt' \
	'cat /source.txt' \
	'cp /source.txt /copy.txt' \
	'stat /copy.txt' \
	'touch /touch.txt' \
	'cd /bin > /cdredir' \
	'pwd' \
	'ls' \
	'cp ../copy.txt ../copy2.txt' \
	'/bin/rm /bin/link' \
	'cp /source.txt /bin/overlay.txt' \
	'stat /bin/overlay.txt' \
	'/bin/basename /usr/bin/tool' \
	'/bin/dirname /usr/bin/tool' \
	'/bin/realpath /bin/../source.txt' \
	'/bin/pathchk -p portable/name' \
	'echo FILE_PATH_TOOLS_PASS' \
	'/bin/uname -a' \
	'/bin/cp /source.txt /cmdcpy' \
	'/bin/cp /source.txt /cmdemp' \
	'/bin/cmp /source.txt /cmdcpy' \
	'/bin/mv /cmdcpy /cmdmov' \
	'/bin/head -n 1 /cmdmov' \
	'/bin/tail -n 1 /cmdmov' \
	'/bin/wc -l /cmdmov' \
	'/bin/cksum /cmdmov' \
	'/bin/df /' \
	'/bin/id' \
	'/bin/kill -l' \
	'/bin/date +DATE_%Y' \
	'/bin/du /source.txt' \
	'/bin/stty -a' \
	'/bin/time /bin/cat /source.txt' \
	'/bin/timeout 2 /bin/cat /source.txt' \
	'/bin/od /source.txt' \
	'/bin/tr a-z A-Z < /source.txt' \
	'/bin/cut -b 1-6 /source.txt' \
	'/bin/paste /source.txt /source.txt' \
	'/bin/sort /source.txt' \
	'/bin/uniq /source.txt' \
	'/bin/comm /source.txt /source.txt' \
	'/bin/fold -w 80 /source.txt' \
	'/bin/fmt -w 80 /source.txt' \
	'/bin/pr -n /source.txt' \
	'/bin/nl /source.txt' \
	'/bin/expand /source.txt' \
	'/bin/unexpand /source.txt' \
	'/bin/grep zedBSD /source.txt' \
	'/bin/sed s/zedBSD/TEXT_SED_PASS/ /source.txt' \
	"/bin/awk '{ print \$1 }' /source.txt" \
	'/bin/xargs /bin/basename < /xarg' \
	'/bin/iconv -f UTF-8 -t UTF-8 /source.txt' \
	'/bin/diff /source.txt /source.txt' \
	'echo TEXT_COMMANDS_PASS' \
	'/bin/touch /cmdmov' \
	'/bin/truncate -s 4 /cmdemp' \
	'/bin/stat /cmdemp' \
	'set PATH /apps:/bin' \
	'basename /test/SHELL_PATH_PASS' \
	'set SHVAR SHELL_VAR_PASS' \
	'echo "$SHVAR"' \
	'echo ${SHVAR}' \
	'LOCAL_ONLY=SHELL_ASSIGN_PASS' \
	'echo "$LOCAL_ONLY"' \
	'export EXPORTED=SHELL_EXPORT_PASS' \
	'echo "$EXPORTED"' \
	'readonly FIXED=SHELL_READONLY_PASS' \
	'echo "$FIXED"' \
	'unset MISSING' \
	'echo ${MISSING:-SHELL_DEFAULT_PASS}' \
	'echo ${SHVAR:+SHELL_ALT_PASS}' \
	'echo ${ASSIGNED:=SHELL_ASSIGNOP_PASS}' \
	'echo "$ASSIGNED"' \
	'echo SHELL_GLOB_PASS /bin/c?t' \
	'echo SHELL_QUOTED_GLOB_PASS "*.none"' \
	'echo SHELL_COMMAND_$(echo SUBSTITUTE_PASS)' \
	'set COUNT 7' \
	'echo SHELL_ARITH_$((COUNT * 6))' \
	'eval echo SHELL_EVAL_PASS' \
	'. /sourced' \
	'read READ_VALUE < /rdinput' \
	'echo "$READ_VALUE"' \
	'type cd' \
	'command basename /tmp/SHELL_COMMAND_PASS' \
	'umask 022' \
	'umask' \
	'unset TMPONLY' \
	'TMPONLY=SHELL_TEMP_PASS env' \
	'echo SHELL_TEMP_${TMPONLY-unset}' \
	'SPECIAL=SHELL_SPECIAL_PASS :' \
	'echo "$SPECIAL"' \
	'printf "SHELL_PRINTF_%s_%d\\n" string 42' \
	'test -f /source.txt && echo SHELL_TEST_FILE_PASS' \
	'[ 7 -gt 3 ] && echo SHELL_TEST_INTEGER_PASS' \
	'alias hi="echo SHELL_ALIAS_PASS"' \
	'hi' \
	'trap "echo SHELL_TRAP_PASS" USR1' \
	'kill -USR1 $$' \
	':' \
	'set OPTIND 1' \
	'getopts ab: OPTION -a -b value' \
	'echo SHELL_GETOPTS_$OPTION' \
	'getopts ab: OPTION -a -b value' \
	'echo SHELL_GETOPTS_${OPTION}_${OPTARG}' \
	'sleep 0 & wait $!' \
	'false; echo SHELL_STATUS_$?' \
	'set OUT /varexp' \
	'echo SHELL_EXPAND_REDIR > "$OUT"' \
	'/bin/cat "$OUT"' \
	'/bin/sh /argtest first second' \
	'echo "SHELL QUOTE PASS"' \
	'true && echo SHELL_AND_PASS' \
	'false || echo SHELL_OR_PASS' \
	'false && echo SHELL_BAD_AND || true' \
	'true || echo SHELL_BAD_OR' \
	'echo ignored; echo SHELL_SEMI_PASS' \
	'echo SHELL_PIPE_PASS | /bin/cat' \
	'echo SHELL_REDIR_PASS > /redir' \
	'echo SHELL_APPEND_PASS >> /redir' \
	'/bin/cat < /redir' \
	'echo USER_COMMANDS_PASS' \
	'clear' \
	'true' \
	'echo SH_BUILTINS_PASS' \
	'halt' >"$work/zinit.rc"

rootfs="$work/rootfs.img"
mcopy -i "$work/test.img@@$offset" ::/rootfs.img "$rootfs"
mmd -i "$rootfs" ::/etc 2>/dev/null || true
mcopy -i "$rootfs" "$work/source.txt" ::/source.txt
mcopy -i "$rootfs" "$work/argtest" ::/argtest
mcopy -i "$rootfs" "$work/sourced" ::/sourced
mcopy -i "$rootfs" "$work/rdinput" ::/rdinput
mcopy -i "$rootfs" "$work/xarginput" ::/xarg
mcopy -i "$rootfs" "$work/zinit.rc" ::/etc/zinit.rc
mcopy -o -i "$work/test.img@@$offset" "$rootfs" ::/rootfs.img

case "$arch" in
pcat)
	qemu="${QEMU_PCAT_I386:-qemu-system-i386}"
	timeout 60 "$qemu" -M pc -cpu 486 -m 64M -display none \
		-serial none -monitor none -no-reboot -no-shutdown \
		-debugcon "file:$work/debug.log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,index=0,media=disk,format=raw,file=$work/test.img" \
		>/dev/null 2>&1 || test "$?" -eq 124
	for marker in 'zedBSD shell builtin copy fixture' SHELL_PATH_PASS \
	    SHELL_VAR_PASS SHELL_ASSIGN_PASS SHELL_EXPORT_PASS \
	    SHELL_READONLY_PASS SHELL_DEFAULT_PASS SHELL_ALT_PASS \
	    SHELL_ASSIGNOP_PASS SHELL_STATUS_1 SHELL_EXPAND_REDIR \
	    'SHELL_GLOB_PASS /bin/cat' 'SHELL_QUOTED_GLOB_PASS *.none' \
	    SHELL_COMMAND_SUBSTITUTE_PASS \
	    SHELL_ARITH_42 \
	    SHELL_EVAL_PASS SHELL_DOT_PASS SHELL_READ_PASS \
	    SHELL_COMMAND_PASS 'shell builtin: cd' '0022' \
	    SHELL_TEMP_PASS SHELL_TEMP_unset SHELL_SPECIAL_PASS \
	    SHELL_PRINTF_string_42 SHELL_TEST_FILE_PASS SHELL_TEST_INTEGER_PASS \
	    SHELL_ALIAS_PASS \
	    SHELL_TRAP_PASS \
	    SHELL_GETOPTS_a SHELL_GETOPTS_b_value \
	    SHELL_ARGC_2 SHELL_ARG1_first SHELL_ARG2_second \
	    SHELL_SHIFT_second \
	    'SHELL QUOTE PASS' \
	    SHELL_AND_PASS SHELL_OR_PASS SHELL_SEMI_PASS SHELL_PIPE_PASS \
	    SHELL_REDIR_PASS SHELL_APPEND_PASS USER_COMMANDS_PASS \
	    /source.txt FILE_PATH_TOOLS_PASS \
	    EXTERNAL_LS_PASS \
	    'ELF 32-bit little-endian, Intel 80386' '/source.txt: text' \
	    DATE_ TEXT_SED_PASS TEXT_XARGS_PASS TEXT_COMMANDS_PASS \
	    'ZEDBSD SHELL BUILTIN COPY FIXTURE' \
	    SH_BUILTINS_PASS; do
		if ! grep -Fq "$marker" "$work/debug.log"; then
			cat "$work/debug.log" >&2
			echo "missing shell test marker: $marker" >&2
			exit 1
		fi
	done
	! grep -Fq SHELL_BAD_AND "$work/debug.log"
	! grep -Fq SHELL_BAD_OR "$work/debug.log"
	grep -Fq '/bin' "$work/debug.log"
	grep -Fq 'type=regular' "$work/debug.log"
	if grep -Fq 'command failed' "$work/debug.log"; then
		cat "$work/debug.log" >&2
		echo "a shell builtin failed" >&2
		exit 1
	fi
	;;
pc98)
	qemu="${QEMU_PC98:-/home/awe/qemu-pc98/build/qemu-system-i386}"
	bios="${PC98_BIOS_DIR:-/home/awe/qemu-pc98/roms/pc98bios}"
	test -x "$qemu" && test -d "$bios"
	timeout 60 "$qemu" -M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios" \
		-nic none -display none -serial none -monitor none -no-reboot \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$work/test.img" \
		>/dev/null 2>&1 || test "$?" -eq 124

	# The scripted zinit.rc path above does not exercise the keyboard.  Boot
	# an unmodified image, type a command through QMP, and verify its FAT
	# side effect so a missing console-input worker cannot pass unnoticed.
	cp --reflink=auto "$build/bios-hdd-image.img" "$work/keyboard.img"
	keyboard_qmp="$work/keyboard.qmp"
	timeout 22 "$qemu" -M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios" \
		-nic none -display none -serial none -monitor none -no-reboot \
		-qmp "unix:$keyboard_qmp,server=on,wait=off" \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$work/keyboard.img" \
		>/dev/null 2>&1 &
	keyboard_pid=$!
	python3 - "$keyboard_qmp" <<'PY'
import json
import socket
import sys
import time

stream = socket.socket(socket.AF_UNIX)
deadline = time.monotonic() + 10
while True:
    try:
        stream.connect(sys.argv[1])
        break
    except (FileNotFoundError, ConnectionRefusedError):
        if time.monotonic() >= deadline:
            raise
        time.sleep(.05)
file = stream.makefile("rw")
json.loads(file.readline())

def qmp(execute, arguments=None, reply=True):
    request = {"execute": execute}
    if arguments is not None:
        request["arguments"] = arguments
    file.write(json.dumps(request) + "\n")
    file.flush()
    if not reply:
        return
    while True:
        response = json.loads(file.readline())
        if "return" in response:
            return response["return"]
        if "error" in response:
            raise SystemExit(f"QMP {execute}: {response['error']}")

def key(qcode):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True,
         "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False,
         "key": {"type": "qcode", "data": qcode}}},
    ]})

qmp("qmp_capabilities")
time.sleep(12)
for qcode in ("t", "o", "u", "c", "h", "spc", "slash", "k", "b",
              "d", "minus", "o", "k"):
    key(qcode)
    time.sleep(.08)
key("ret")
time.sleep(3)
qmp("quit", reply=False)
file.close()
PY
	wait "$keyboard_pid" 2>/dev/null || true
	keyboard_pid=
	mcopy -i "$work/keyboard.img@@$offset" ::/rootfs.img \
		"$work/keyboard-rootfs.img"
	mdir -i "$work/keyboard-rootfs.img" ::/kbd-ok >/dev/null
	;;
esac

result_rootfs="$work/result-rootfs.img"
mcopy -i "$work/test.img@@$offset" ::/rootfs.img "$result_rootfs"
for name in copy.txt copy2.txt; do
	mcopy -i "$result_rootfs" "::/$name" "$work/$name"
	cmp "$work/source.txt" "$work/$name"
done
mcopy -i "$result_rootfs" ::/touch.txt "$work/touch.txt"
test ! -s "$work/touch.txt"

mcopy -i "$result_rootfs" ::/cmdmov \
	"$work/command-moved.txt"
cmp "$work/source.txt" "$work/command-moved.txt"
mcopy -i "$result_rootfs" ::/cmdemp \
	"$work/command-empty.txt"
test "$(stat -c %s "$work/command-empty.txt")" -eq 4

# /bin belongs directly to the selected root filesystem image.
mcopy -i "$result_rootfs" ::/bin/overlay.txt "$work/overlay.txt"
cmp "$work/source.txt" "$work/overlay.txt"

echo "zedBSD /bin/sh builtin QEMU test: PASS ($arch)"

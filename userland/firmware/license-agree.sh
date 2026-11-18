#!/bin/sh
# Ask once whether the license of a firmware package is accepted.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: license-agree.sh PACKAGE RECORD NOTICE LICENSE-NAME SIZE SHA-256 \
#            REPOSITORY-URL UPSTREAM-URL FETCH-COMMAND...
#        ZEDBSD_FIRMWARE_LICENSE_MODE=pending license-agree.sh (the same arguments)
#        ZEDBSD_FIRMWARE_LICENSE_MODE=accept license-agree.sh PACKAGE RECORD LICENSE-NAME SIZE SHA-256
#
# RECORD lists the accepted license texts, one "SHA-256  NAME" line each.
# When it already names SHA-256 nothing is shown.  Otherwise the license text
# is fetched (from REPOSITORY-URL/LICENSE-NAME first when REPOSITORY-URL is
# not empty, then from UPSTREAM-URL), checked against SIZE and SHA-256, and
# kept beside RECORD in licenses/LICENSE-NAME.SHA-256.  Then the standard
# error shows what the firmware is and its copyright (NOTICE), the whole
# license text, and the question, and one line is read from the standard
# input: "yes" or "y" in any case accepts and appends to RECORD, anything
# else, an empty line or the end of the input refuses.  The standard input is
# not waited on when it is not a terminal: a pipe that gives no line within a
# few seconds is a refusal, so an unattended build fails instead of hanging.
# Nothing is written to the standard output, so make may run this from
# $(shell).  The exit status is 0 when the license is accepted.
#
# Two modes serve a front end that asks in its own way (tools/menuconfig.py,
# plan/ws233/phase002/phase.md) and never ask anything:
#   pending  fetches and checks the license text as above when RECORD does not
#            name it, and prints one line, PACKAGE NOTICE LICENSE-TEXT RECORD
#            LICENSE-NAME SIZE SHA-256 separated by tabs; nothing when it does.
#   accept   records the acceptance of the text pending kept, after checking
#            it again.  Only this script writes RECORD.
#
# One lock beside the cache roots keeps two packages (make -j) from asking at
# the same time.
set -eu

mode=${ZEDBSD_FIRMWARE_LICENSE_MODE:-ask}
case $mode in
ask | pending) minimum=9 ;;
accept) minimum=5 ;;
*)
	echo "license-agree: unknown ZEDBSD_FIRMWARE_LICENSE_MODE: $mode" >&2
	exit 2
	;;
esac
if test $# -lt $minimum; then
	echo "license-agree: too few arguments" >&2
	exit 2
fi
package=$1
record=$2
if test "$mode" = accept; then
	notice=
	name=$3
	size=$4
	hash=$5
	repository=
	upstream=
	shift 5
else
	notice=$3
	name=$4
	size=$5
	hash=$6
	repository=$7
	upstream=$8
	shift 8
fi

accepted() {
	test -f "$record" && grep -qx "$hash  $name" "$record"
}

if accepted; then
	exit 0
fi

root=$(dirname -- "$record")
mkdir -p "$root/licenses"
if command -v flock >/dev/null 2>&1; then
	exec 9>>"$(dirname -- "$root")/.license-agree.lock"
	flock 9
fi
if accepted; then
	exit 0
fi

text=$root/licenses/$name.$hash
valid() {
	test -f "$1" && ! test -L "$1" &&
		test "$(wc -c <"$1" | tr -d '[:space:]')" = "$size" &&
		test "$(sha256sum "$1" | awk '{print $1}')" = "$hash"
}
record_acceptance() {
	printf '%s  %s\n' "$hash" "$name" >>"$record"
}
if test "$mode" = accept; then
	if ! valid "$text"; then
		echo "$package: the license text $text that was shown is missing or changed; nothing recorded" >&2
		exit 1
	fi
	record_acceptance
	exit 0
fi
if ! valid "$text"; then
	part=$text.part
	if test -z "$repository" || ! "$@" --output "$part" "$repository/$name"; then
		if test -n "$repository"; then
			echo "$package: could not fetch $name from the zedBSD firmware repository; fetching it from upstream" >&2
		fi
		"$@" --output "$part" "$upstream"
	fi
	if ! valid "$part"; then
		echo "$package: the license text $name does not have the declared size and SHA-256" >&2
		exit 1
	fi
	mv -- "$part" "$text"
fi

if test "$mode" = pending; then
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$package" "$notice" "$text" "$record" "$name" "$size" "$hash"
	exit 0
fi

{
	printf '\n========================================================================\n'
	printf ' %s: firmware license\n' "$package"
	printf '========================================================================\n\n'
	cat -- "$notice"
	printf '\n%s\n' '------------------------------------------------------------------------'
	printf ' License (%s)\n' "$name"
	printf '%s\n\n' '------------------------------------------------------------------------'
	cat -- "$text"
	printf '\n%s\n' '------------------------------------------------------------------------'
	printf 'Do you accept the license of %s? [yes/no] ' "$package"
} >&2

answer=
if test -t 0; then
	IFS= read -r answer || :
elif command -v timeout >/dev/null 2>&1; then
	answer=$(timeout 5 sh -c 'IFS= read -r line || test -n "$line"; printf "%s" "$line"' 2>/dev/null) || :
else
	IFS= read -r answer || :
fi
answer=$(printf '%s' "$answer" | tr -d '[:space:]' | tr '[:upper:]' '[:lower:]')
printf '%s\n' "$answer" >&2

case $answer in
yes | y)
	record_acceptance
	echo "$package: license accepted" >&2
	;;
*)
	if test -z "$answer"; then
		reason='no answer on the standard input'
	else
		reason="the answer was \"$answer\""
	fi
	cat >&2 <<EOF
$package: the license was not accepted ($reason), so the firmware is not fetched.
  To accept it, answer yes: run "make firmware-licenses" in a terminal, or in a
  script or CI run "yes | make firmware-licenses" before the build.
  To build without it, remove $package from ZEDBSD_USER_PROGRAMS.
EOF
	exit 1
	;;
esac

#!/bin/sh
# ws194-p001: tools/build/keiland-prerequisites.sh against stand-in package managers on the host.
#
# Each case puts stand-ins (dpkg-query, apt-get, rpm, dnf, pacman, pkg, sudo, id, make) first on a PATH that holds
# nothing else but the few tools the script uses, answers its question through a pseudo-terminal (script(1)) or
# runs it without one, and checks what it printed, what it ran and how it ended.  The last line is
# prerequisites-host-test: PASS or FAIL.  Output: build/ws194-host (a fresh directory each run).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

set -u
cd "$(dirname "$0")/../../.." || exit 2
. plan/tools/fresh-out.sh
fresh_out build/ws194-host
out=$(cd build/ws194-host && pwd -P)
script_path=$PWD/tools/build/keiland-prerequisites.sh
terminal_tool=$(command -v script)
session_tool=$(command -v setsid)
status=0

# The tools the script needs besides the stand-ins.
mkdir -p "$out/tools"
for tool in sh grep cat id; do
	ln -s "$(command -v $tool)" "$out/tools/$tool"
done

# Writes a stand-in: one that records its arguments in calls.log, and for a package query answers from installed.
stand_in() {
	mkdir -p "$out/$1"
	cat > "$out/$1/$2" <<STUB
#!/bin/sh
echo "$2 \$*" >> "$out/calls.log"
case "$2" in
dpkg-query) grep -qx "\$3" "$out/installed" && printf 'install ok installed' ; exit 0 ;;
rpm) grep -qx "\$2" "$out/installed" ;;
pacman) grep -qx "\$2" "$out/installed" ;;
pkg) [ "\$1" = info ] && grep -qx "\$3" "$out/installed" ;;
id) echo 1000 ;;
sudo) shift 0; exit 0 ;;
*) exit 0 ;;
esac
STUB
	chmod +x "$out/$1/$2"
}

for name in dpkg-query apt-get sudo id make; do stand_in apt "$name"; done
for name in rpm dnf sudo id; do stand_in dnf "$name"; done
for name in pacman sudo id; do stand_in pacman "$name"; done
for name in pkg sudo id make; do stand_in pkg "$name"; done

# Runs the script with a manager's stand-ins: $1 manager, $2 terminal (yes: a pseudo-terminal it reads and writes;
# job: a controlling pseudo-terminal, but the standard input and output not it, as a BSD make -j job has them,
# T1-498; no: no controlling terminal at all, setsid(1)), $3 answer, then its arguments.
run() {
	manager=$1
	terminal=$2
	answer=$3
	shift 3
	: > "$out/calls.log"
	if [ "$terminal" = yes ]; then
		printf '%s\n' "$answer" | PATH="$out/$manager:$out/tools" KEILAND_ASK="${ASK:-y}" \
			"$terminal_tool" -qec "sh $script_path $*" /dev/null > "$out/output" 2>&1
	elif [ "$terminal" = job ]; then
		printf '%s\n' "$answer" | PATH="$out/$manager:$out/tools" KEILAND_ASK="${ASK:-y}" \
			"$terminal_tool" -qec "sh $script_path $* < /dev/null > $out/output 2>&1" /dev/null > "$out/terminal" 2>&1
	else
		PATH="$out/$manager:$out/tools" KEILAND_ASK="${ASK:-y}" "$session_tool" -w sh "$script_path" "$@" \
			> "$out/output" 2>&1 < /dev/null
	fi
	echo $?
}

# Reports one check.
expect() {
	if [ "$2" = 0 ]; then
		echo "$1: ok"
	else
		echo "$1: FAIL"
		status=1
	fi
}

# apt: everything installed.
printf '%s\n' build-essential libvulkan-dev linux-libc-dev python3 curl > "$out/installed"
code=$(run apt no "" check linux)
test "$code" = 0 && test ! -s "$out/output"
expect "apt, all there: nothing asked, success" $?

# apt: two missing, no terminal: listed with the command, stops.
printf '%s\n' build-essential linux-libc-dev python3 > "$out/installed"
code=$(run apt no "" check linux)
test "$code" = 1 && grep -q 'libvulkan-dev curl' "$out/output" && grep -q 'sudo apt-get install -y libvulkan-dev curl' "$out/output" && ! grep -q '^apt-get' "$out/calls.log"
expect "apt, missing, no terminal: listed, nothing installed, stops" $?

# apt: missing, a terminal, yes: installed with sudo.
code=$(run apt yes y check linux)
test "$code" = 0 && grep -qx 'sudo apt-get install -y libvulkan-dev curl' "$out/calls.log"
expect "apt, missing, terminal, yes: installed with sudo" $?

# apt: missing, a terminal, no: nothing installed, stops.
code=$(run apt yes n check linux)
test "$code" = 1 && ! grep -q 'apt-get' "$out/calls.log"
expect "apt, missing, terminal, no: nothing installed, stops" $?

# apt: KEILAND_ASK=n on a terminal: no question, stops.
code=$(ASK=n run apt yes y check linux)
test "$code" = 1 && ! grep -q 'Install them now' "$out/output" && ! grep -q 'apt-get' "$out/calls.log"
expect "KEILAND_ASK=n: nothing asked, stops" $?

# dnf and pacman: the missing ones in their names.
: > "$out/installed"
code=$(run dnf no "" check linux)
test "$code" = 1 && grep -q 'sudo dnf install -y gcc make vulkan-headers vulkan-loader-devel kernel-headers python3 curl' "$out/output"
expect "dnf: its package names" $?
code=$(run pacman no "" check linux)
test "$code" = 1 && grep -q 'sudo pacman -S --needed --noconfirm base-devel vulkan-headers vulkan-icd-loader linux-api-headers python curl' "$out/output"
expect "pacman: its package names" $?

# FreeBSD pkg: missing gmake, a terminal, yes.
printf '%s\n' python3 meson ninja vulkan-headers vulkan-loader libdrm mesa-dri seatd > "$out/installed"
code=$(run pkg yes y check freebsd)
test "$code" = 0 && grep -qx 'sudo pkg install -y gmake' "$out/calls.log"
expect "pkg, gmake missing, terminal, yes: installed" $?

# FreeBSD pkg: seatd missing, asked on the controlling terminal although make -j gave the job no terminal on its
# standard input and output (T1-498: BSD make -j8 printed the command and stopped).
printf '%s\n' gmake python3 meson ninja vulkan-headers vulkan-loader libdrm mesa-dri > "$out/installed"
code=$(run pkg job y check freebsd)
test "$code" = 0 && grep -qx 'sudo pkg install -y seatd' "$out/calls.log" && grep -q 'Install them now' "$out/terminal"
expect "pkg, seatd missing, make -j job on a terminal, yes: asked on /dev/tty, installed" $?

# The install after the build: no terminal prints, a terminal and yes runs it with sudo, no does not.
code=$(run apt no "" offer-install linux make -f userland/desktop/keiland-linux.mk install)
test "$code" = 0 && grep -q 'Install Keiland with: sudo make -f userland/desktop/keiland-linux.mk install' "$out/output" && ! grep -q '^sudo' "$out/calls.log"
expect "install, no terminal: printed only" $?
code=$(run apt yes y offer-install linux make -f userland/desktop/keiland-linux.mk install)
test "$code" = 0 && grep -qx 'sudo make -f userland/desktop/keiland-linux.mk install' "$out/calls.log"
expect "install, terminal, yes: run with sudo" $?
code=$(run apt yes n offer-install linux make -f userland/desktop/keiland-linux.mk install)
test "$code" = 0 && ! grep -q '^sudo' "$out/calls.log"
expect "install, terminal, no: not run, still success" $?

if [ "$status" -eq 0 ]; then
	echo "prerequisites-host-test: PASS"
else
	echo "prerequisites-host-test: FAIL (output in $out)"
fi
exit "$status"

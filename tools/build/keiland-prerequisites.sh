#!/bin/sh
# The host packages the native Keiland builds need, and the install after a build (WS194, the 2026-10-09 user
# request): "make keiland-linux" and "make keiland-freebsd" run this before and after the build.
#
#   keiland-prerequisites.sh check linux|freebsd
#       Lists the packages the build needs that the host's package manager (apt, dnf or yum, pacman; FreeBSD's pkg)
#       does not have installed.  With a controlling terminal (/dev/tty) it asks before installing them (y/N) and
#       installs them with sudo; without one, or when the answer is no, it prints what is missing and how to install
#       it, and fails, so that the build does not start half equipped.  With everything there it says nothing and
#       succeeds.
#   keiland-prerequisites.sh offer-install linux|freebsd COMMAND...
#       After a build that succeeded: with a controlling terminal it asks whether to install now (y/N) and runs
#       COMMAND (with sudo unless it runs as root); without one it prints COMMAND and succeeds.
#
# KEILAND_ASK=n in the environment (the make variable of the same name) asks nothing: missing packages are listed and
# the build stops, and the install is only printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

set -u

# Prints a line on the standard error.
say() {
	printf '%s\n' "$*" >&2
}

# Succeeds when the questions may be asked: a controlling terminal to ask on, and KEILAND_ASK not n.  The terminal
# is /dev/tty, not the standard input and output: BSD make -j (FreeBSD's make -j8, which BSDmakefile turns into gmake)
# gives a job neither (T1-498), and GNU make -j gives the standard input to one job only.
interactive() {
	[ "${KEILAND_ASK:-y}" != n ] || return 1
	# In a subshell: a redirection that fails on a special built-in ends the shell that runs it.
	( : </dev/tty >/dev/tty ) 2>/dev/null || return 1
	foreground
}

# Succeeds unless this runs in a process group that is not the terminal's foreground one: a read of /dev/tty there
# stops the job (SIGTTIN) or fails with EIO (T1-503: FreeBSD's make -j8 runs each job in a process group of its own,
# "read: read error: Input/output error").  BSDmakefile runs its targets in the foreground (.MAKEFLAGS: -B); this is
# the guard for any other make that does not.  When ps cannot tell, the questions are asked.
foreground() {
	group=$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ')
	terminal_group=$(ps -o tpgid= -p $$ 2>/dev/null | tr -d ' ')
	[ -n "$group" ] && [ -n "$terminal_group" ] || return 0
	[ "$group" = "$terminal_group" ] && return 0
	say "keiland: this runs in the background of the terminal (a job of make -j), where it cannot ask."
	return 1
}

# Asks a yes-or-no question on the terminal; succeeds on yes.  The default (Enter) is no.
ask() {
	printf '%s [y/N] ' "$1" >/dev/tty
	read -r answer </dev/tty || return 1
	case $answer in
	y | Y | yes | YES | Yes)
		return 0
		;;
	esac
	return 1
}

# The way to run a command as root: nothing as root, else sudo, else doas.
as_root() {
	if [ "$(id -u)" = 0 ]; then
		echo ""
	elif command -v sudo >/dev/null 2>&1; then
		echo sudo
	elif command -v doas >/dev/null 2>&1; then
		echo doas
	else
		echo sudo
	fi
}

# Finds the package manager and sets manager, wanted (the packages the build needs), the test of one being installed
# and the command that installs them.
find_manager() {
	system=$1
	manager=
	if [ "$system" = freebsd ]; then
		if command -v pkg >/dev/null 2>&1; then
			manager=pkg
			wanted="gmake python3 meson ninja vulkan-headers vulkan-loader libdrm mesa-dri seatd"
			install="pkg install -y"
		fi
		return 0
	fi
	if command -v dpkg-query >/dev/null 2>&1 && command -v apt-get >/dev/null 2>&1; then
		manager=apt
		wanted="build-essential libvulkan-dev linux-libc-dev python3 curl"
		install="apt-get install -y"
	elif command -v dnf >/dev/null 2>&1; then
		manager=dnf
		wanted="gcc make vulkan-headers vulkan-loader-devel kernel-headers python3 curl"
		install="dnf install -y"
	elif command -v yum >/dev/null 2>&1; then
		manager=yum
		wanted="gcc make vulkan-headers vulkan-loader-devel kernel-headers python3 curl"
		install="yum install -y"
	elif command -v pacman >/dev/null 2>&1; then
		manager=pacman
		wanted="base-devel vulkan-headers vulkan-icd-loader linux-api-headers python curl"
		install="pacman -S --needed --noconfirm"
	fi
}

# Succeeds when one package is installed, as its manager reports it.
installed() {
	case $manager in
	apt)
		dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'install ok installed'
		;;
	dnf | yum)
		rpm -q "$1" >/dev/null 2>&1
		;;
	pacman)
		pacman -Q "$1" >/dev/null 2>&1
		;;
	pkg)
		pkg info -e "$1" >/dev/null 2>&1
		;;
	*)
		return 1
		;;
	esac
}

# check: the packages first.
check() {
	find_manager "$1"
	if [ -z "$manager" ]; then
		say "keiland: no known package manager (apt, dnf, yum, pacman, pkg); make sure a C compiler, make, the Vulkan"
		say "keiland: loader and headers, the kernel headers, python3 and curl are installed."
		return 0
	fi
	missing=
	for package in $wanted; do
		if ! installed "$package"; then
			missing="$missing $package"
		fi
	done
	missing=${missing# }
	if [ -z "$missing" ]; then
		return 0
	fi
	root=$(as_root)
	command="${root:+$root }$install $missing"
	echo "Keiland needs these $manager packages, which are not installed:"
	echo "  $missing"
	if interactive && ask "Install them now with: $command ?"; then
		if $command; then
			return 0
		fi
		say "keiland: installing the packages failed."
		return 1
	fi
	say "keiland: install them with: $command"
	say "keiland: then run the build again."
	return 1
}

# offer-install: the install after a good build.
offer_install() {
	shift
	root=$(as_root)
	command="${root:+$root }$*"
	if interactive && ask "The build succeeded.  Install Keiland now with: $command ?"; then
		if $command; then
			return 0
		fi
		say "keiland: the install failed."
		return 1
	fi
	echo "The build succeeded.  Install Keiland with: $command"
	return 0
}

case ${1:-} in
check)
	[ $# -eq 2 ] || { say "usage: keiland-prerequisites.sh check linux|freebsd"; exit 2; }
	check "$2"
	;;
offer-install)
	[ $# -ge 3 ] || { say "usage: keiland-prerequisites.sh offer-install linux|freebsd COMMAND..."; exit 2; }
	shift
	offer_install "$@"
	;;
*)
	say "usage: keiland-prerequisites.sh check|offer-install ..."
	exit 2
	;;
esac

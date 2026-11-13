#!/bin/sh
# Builds a test image the one standard way (ws136-p001, 2026-10-04, the user's decision: a test image is a build with
# the test's configuration plus individual files copied into it, and nothing an older build left in build/): make
# with the test's configuration and BUILD, and the files copied into the root before the image is made.  The files'
# sources are in the tree (the test's tests/, userland/ and the like) or are what this build itself makes.
#
#   plan/tools/guest/test-image.sh [--no-harness] CONFIG BUILD [ARGUMENT...]
#
# Each ARGUMENT is one of:
#   --file DEST=SOURCE    a file to copy into the root (two words; DEST is absolute)
#   --mode DEST=MODE      its permissions (two words)
#   NAME=VALUE            a make variable
#   TARGET                a make target (default disk; the old name disk-image is taken as disk)
# CONFIG is given to make as CONFIGMK.  The guest harness's files (the SSH keys and net.conf,
# plan/tools/guest/guest.py extra-files) are added unless --no-harness is given.  ZEDBSD_JOBS sets the parallel jobs
# of make and of the build tools it runs (BUILD_JOBS; plan/tools/guest/jobs.sh).
#
# The files are copied into BUILD/rootfs after make has made the root and before it makes the image, so they take
# the place of a file of the same name that a package installed (the root's rule copies the build's own files over
# the files its lists name, so a list could not replace one).  The root's stamp is then touched, so that the image is
# made again from it.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
. plan/tools/guest/jobs.sh
harness=y
if [ "${1:-}" = --no-harness ]; then
	harness=n
	shift
fi
[ $# -ge 2 ] || { echo "usage: test-image.sh [--no-harness] CONFIG BUILD [ARGUMENT...]" >&2; exit 2; }
config=$1
build=${2%/}
shift 2
[ -f "$config" ] || { echo "test-image: no config $config" >&2; exit 1; }
files=
if [ $harness = y ]; then
	files=$(python3 plan/tools/guest/guest.py extra-files)
	[ -n "$files" ] || { echo "test-image: no guest harness files (plan/tools/guest/guest.py keys?)" >&2; exit 1; }
fi
variables=
targets=
while [ $# -gt 0 ]; do
	case $1 in
	--file|--mode)
		[ $# -ge 2 ] || { echo "test-image: $1 needs DEST=VALUE" >&2; exit 2; }
		case $2 in
		/*=?*) ;;
		*) echo "test-image: $1 $2: not DEST=VALUE with an absolute DEST" >&2; exit 2 ;;
		esac
		files="$files $1 $2"
		shift 2
		;;
	*=*)
		variables="$variables
$1"
		shift
		;;
	disk-image)
		targets="$targets disk"
		shift
		;;
	*)
		targets="$targets $1"
		shift
		;;
	esac
done
[ -n "$targets" ] || targets=disk

# Runs make with the configuration, the build directory, the jobs and the
# variables (one per line, so that a value may hold spaces), then the
# arguments given.
run_make() {
	(
		set -- make -j"$ZEDBSD_JOBS" BUILD_JOBS="$ZEDBSD_JOBS" CONFIGMK="$config" BUILD="$build" "$@"
		IFS='
'
		for variable in $variables; do
			set -- "$@" "$variable"
		done
		IFS='
'
		exec "$@"
	)
}

# The firmware the configuration selects is fetched after its license is accepted once (ws233-p001); a test image is
# built unattended, so the question is answered the way CI answers it (2026-10-14, Q1 for the user).  Only the
# platforms with such firmware have the target.
if run_make -n firmware-licenses >/dev/null 2>&1; then
	yes | run_make firmware-licenses
fi

if [ -n "$files" ]; then
	run_make rootfs
	root=$build/rootfs
	[ -f "$root/.stamp" ] || { echo "test-image: make rootfs left no $root/.stamp" >&2; exit 1; }
	# Every file first, then every mode, as the root's own lists are put in.
	for pass in file mode; do
		# shellcheck disable=SC2086
		set -- $files
		while [ $# -ge 2 ]; do
			option=$1
			destination=$root${2%%=*}
			value=${2#*=}
			shift 2
			case $option:$pass in
			--file:file)
				[ -f "$value" ] || { echo "test-image: --file: no $value" >&2; exit 1; }
				(umask 022; mkdir -p "$(dirname -- "$destination")")
				cp -f "$value" "$destination"
				;;
			--mode:mode)
				[ -e "$destination" ] || {
					echo "test-image: --mode names nothing in the root: ${destination#"$root"}" >&2
					exit 1
				}
				chmod "$value" "$destination"
				;;
			esac
		done
	done
	touch "$root/.stamp"
fi

# shellcheck disable=SC2086
run_make $targets
sync

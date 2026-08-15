#!/bin/sh
# Run a bounded XEiJ X68030 firmware/media smoke test and optionally capture it.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

set -eu

usage()
{
	cat >&2 <<'EOF'
usage: run-x68k-xeij-smoke.sh --xeij-dir DIR --java JAVA \
    --iplrom30 FILE --image FILE [--seconds N] [--screenshot FILE]
EOF
	exit 2
}

xeij_dir=
java_bin=
iplrom30=
image=
seconds=12
screenshot=
while [ "$#" -gt 0 ]; do
	case "$1" in
	--xeij-dir) xeij_dir=$2; shift 2 ;;
	--java) java_bin=$2; shift 2 ;;
	--iplrom30) iplrom30=$2; shift 2 ;;
	--image) image=$2; shift 2 ;;
	--seconds) seconds=$2; shift 2 ;;
	--screenshot) screenshot=$2; shift 2 ;;
	*) usage ;;
	esac
done

[ -d "$xeij_dir" ] || usage
[ -x "$java_bin" ] || usage
[ -f "$iplrom30" ] || usage
[ -f "$image" ] || usage
case "$seconds" in
''|*[!0-9]*) usage ;;
esac
[ "$seconds" -gt 0 ] || usage
command -v xvfb-run >/dev/null 2>&1 || {
	echo "xvfb-run is required" >&2
	exit 1
}
if [ -n "$screenshot" ]; then
	command -v import >/dev/null 2>&1 || {
		echo "ImageMagick import is required for --screenshot" >&2
		exit 1
	}
	screenshot=$(readlink -f "$screenshot")
fi

xeij_dir=$(readlink -f "$xeij_dir")
java_bin=$(readlink -f "$java_bin")
iplrom30=$(readlink -f "$iplrom30")
image=$(readlink -f "$image")
work=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-x68k-xeij.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
ln -s "$image" "$work/zedbsd-x68k.hds"

xvfb-run -a sh -c '
	cd "$1"
	"$2" --enable-native-access=ALL-UNNAMED \
		-cp XEiJ.jar:jSerialComm-2.11.4.jar xeij.XEiJ \
		-ini="$3/xeij.ini" -config=default -saveonexit=off \
		-lang=en -model=X68030 -memory=12 -highmemory=16 -sound=off \
		-flrinformed=on -flrenabled=off \
		-rom30="$4" -boot="$3/zedbsd-x68k.hds" \
		>"$3/xeij.log" 2>&1 &
	pid=$!
	sleep "$5"
	if ! kill -0 "$pid" 2>/dev/null; then
		wait "$pid" || true
		cat "$3/xeij.log" >&2
		exit 1
	fi
	if [ -n "$6" ]; then
		import -window root "$6"
	fi
	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	cat "$3/xeij.log"
' sh "$xeij_dir" "$java_bin" "$work" "$iplrom30" "$seconds" "$screenshot" |
	tee "$work/console.log"

grep -F "was connected to sc0" "$work/console.log" >/dev/null || {
	echo "XEiJ did not attach the image as SCSI ID 0" >&2
	exit 1
}
echo "XEiJ X68030 SCSI media attach: PASS"

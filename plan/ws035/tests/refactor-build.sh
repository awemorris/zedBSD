#!/bin/sh
# ws035 refactor Phases: one build in a private directory of the Phase.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: sh plan/ws035/tests/refactor-build.sh <phase> <name> <config.mk> <target...>
#   phase    p002, p003, ... ; the build goes to build/ws035-<phase>/<name>/
#            and the log to plan/ws035/phase<NNN>/build/<name>.log
#   config   an existing configuration (config/ci/*.mk, plan/ws029/tests/...)
# Environment: DRY=1 prints the make plan only (make -n); JOBS (default 16);
#              LIMIT seconds (default 1800); EXTRA extra make arguments.
#
# LLVM is the checkout's own build/llvm (the Makefile default).  The sysroots
# are built privately under build/ws035-<phase>/sysroot/.  The host Noct
# interpreter is a copy (cp -a, stamps kept) of the ws035-p001 private
# toolchain (baseline-toolchain.sh), so no shared build/ path is written.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
phase=$1; name=$2; config=$3; shift 3
root=build/ws035-$phase
out=$root/$name
logdir=$repo/plan/ws035/phase${phase#p}/build
noct=$repo/$root/noct
sysroot=$repo/$root/sysroot
jobs=${JOBS:-16}
limit=${LIMIT:-1800}
mkdir -p "$repo/$out" "$logdir"
if ! test -d "$noct"; then
	src=$repo/build/ws035-p001/toolchain
	test -d "$src/NoctLang"
	mkdir -p "$noct.tmp"
	cp -a "$src/NoctLang" "$src/host-noct-state" "$src/noct-distfiles" "$noct.tmp/"
	mv "$noct.tmp" "$noct"
fi
set -- \
	BUILD="$out" \
	ZEDBSD_CONFIG="$config" \
	ZEDBSD_SYSROOT_AMD64="$sysroot/amd64" \
	ZEDBSD_SYSROOT_I386="$sysroot/i386" \
	NOCT_HOST_SOURCE_DIR="$noct/NoctLang" \
	NOCT_HOST_STATE_DIR="$noct/host-noct-state" \
	ZEDBSD_NOCT_DISTDIR="$noct/noct-distfiles" \
	${EXTRA:-} "$@"
arch=$(sed -n 's/^ZEDBSD_ARCHITECTURE := //p' "$repo/$config")
case $arch in
amd64) set -- ZEDBSD_TARGET_SYSROOT="$sysroot/amd64" "$@" ;;
i386) set -- ZEDBSD_TARGET_SYSROOT="$sysroot/i386" "$@" ;;
esac
cd "$repo"
if [ "${DRY:-0}" = 1 ]; then
	exec make -n "$@"
fi
log=$logdir/$name.log
start=$(date +%s)
status=0
{
	echo "# ws035-$phase build $name: $(date -Is)"
	echo "# git HEAD $(git rev-parse HEAD) (working tree has the $phase changes)"
	echo "# make -j$jobs $*"
} > "$log"
timeout "$limit" make -j"$jobs" "$@" >> "$log" 2>&1 || status=$?
end=$(date +%s)
warnings=$(grep -c -E '(^|[^A-Za-z])warning:' "$log" || true)
errors=$(grep -c -E '(^|[^A-Za-z])error:|\*\*\* ' "$log" || true)
printf '%s status=%s seconds=%s warnings=%s error_lines=%s\n' \
	"$name" "$status" "$((end - start))" "$warnings" "$errors" | tee -a "$log"
exit "$status"

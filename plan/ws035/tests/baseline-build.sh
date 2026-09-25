#!/bin/sh
# ws035-p001: one pre-refactor baseline build in a private build directory.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: sh plan/ws035/tests/baseline-build.sh <name> <config.mk> <target...>
#   name     label; the build goes to build/ws035-p001/<name>/ and the log to
#            plan/ws035/phase001/baseline/<name>.log
#   config   an existing configuration (config/ci/*.mk, plan/ws029/tests/...)
# Environment: DRY=1 prints the make plan only (make -n); JOBS (default 16);
#              LIMIT seconds (default 1800); EXTRA extra make arguments.
#
# The toolchain, sysroots and Noct come from build/ws035-p001/toolchain
# (baseline-toolchain.sh); nothing is written to another tree or to build/<arch>.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
name=$1; config=$2; shift 2
tc=$repo/build/ws035-p001/toolchain
out=$repo/build/ws035-p001/$name
logdir=$repo/plan/ws035/phase001/baseline
mkdir -p "$out" "$logdir"
jobs=${JOBS:-16}
limit=${LIMIT:-1800}
set -- \
	BUILD="build/ws035-p001/$name" \
	ZEDBSD_CONFIG="$config" \
	ZEDBSD_LLVM_INSTALL="$tc/llvm" \
	ZEDBSD_LLVM_BUILD="$tc/llvm-build" \
	ZEDBSD_LLVM_SOURCE="$tc/llvm-source" \
	ZEDBSD_LLVM_DISTDIR="$tc/llvm-distfiles" \
	ZEDBSD_TARGET_LLVM_BIN="$tc/llvm/bin" \
	ZEDBSD_SYSROOT_AMD64="$tc/sysroot/amd64" \
	ZEDBSD_SYSROOT_I386="$tc/sysroot/i386" \
	NOCT_HOST_SOURCE_DIR="$tc/NoctLang" \
	NOCT_HOST_STATE_DIR="$tc/host-noct-state" \
	ZEDBSD_NOCT_DISTDIR="$tc/noct-distfiles" \
	${EXTRA:-} "$@"
arch=$(sed -n 's/^ZEDBSD_ARCHITECTURE := //p' "$repo/$config")
case $arch in
amd64) set -- ZEDBSD_TARGET_SYSROOT="$tc/sysroot/amd64" "$@" ;;
i386) set -- ZEDBSD_TARGET_SYSROOT="$tc/sysroot/i386" "$@" ;;
esac
cd "$repo"
if [ "${DRY:-0}" = 1 ]; then
	exec make -n "$@"
fi
log=$logdir/$name.log
start=$(date +%s)
status=0
{
	echo "# ws035-p001 baseline $name: $(date -Is)"
	echo "# git HEAD $(git rev-parse HEAD)"
	echo "# make -j$jobs $*"
} > "$log"
timeout "$limit" make -j"$jobs" "$@" >> "$log" 2>&1 || status=$?
end=$(date +%s)
warnings=$(grep -c -E '(^|[^A-Za-z])warning:' "$log" || true)
errors=$(grep -c -E '(^|[^A-Za-z])error:|\*\*\* ' "$log" || true)
printf '%s status=%s seconds=%s warnings=%s error_lines=%s\n' \
	"$name" "$status" "$((end - start))" "$warnings" "$errors" | tee -a "$log"
exit "$status"

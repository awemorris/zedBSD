#!/bin/sh
# Builds libkeiland and the named zedBSD programs for one architecture and counts the warnings.
# Run from the worktree's root.  Writes into build/ws327-NAME-ARCH/ and build/ws327-NAME-ARCH.log.
#
#   sh plan/ws327/tests/build-zedbsd.sh NAME ARCH "PROGRAM..."    # ARCH: amd64, arm64 or riscv64
set -u
name=$1
arch=$2
programs=$3
case $arch in
amd64) config=config/ci/config-amd64.mk ;;
arm64) config=config/ci/config-odroid-c5-keiland.mk ;;
riscv64) config=config/ci/config-visionfive2-keiland.mk ;;
*) echo "unknown arch $arch"; exit 2 ;;
esac
dir=build/ws327-$name-$arch
log=$dir.log
targets=$dir/dynamic/libkeiland.so
for p in $programs; do
	targets="$targets $dir/bin/$p"
done
timeout 1800 make -j32 ZEDBSD_CONFIG="$config" BUILD="$dir" "sysroot-$arch" > "$log" 2>&1
timeout 3600 make -j32 ZEDBSD_CONFIG="$config" BUILD="$dir" $targets >> "$log" 2>&1
status=$?
warnings=$(grep -c 'warning:' "$log")
echo "zedbsd $arch make=$status warnings=$warnings"

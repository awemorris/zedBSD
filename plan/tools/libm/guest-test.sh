#!/bin/sh
# Builds the libm test runner for zedBSD (amd64), puts it and the reference
# cases into a lean image, boots the image and runs the runner in the guest
# over the serial console (WS076).
#
#   plan/tools/libm/guest-test.sh [--count N] [NAME...]
#
# BUILD is the build directory (default build/ws076-amd64) and RUN the
# emulator's work directory (default build/ws076-serial-run).  The runner is
# linked against the image's own libc.so, so it measures the library the
# guest really has.  The image config is plan/tools/libm/config-amd64-libm.mk
# (the lean base image with the serial mirror).
#
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
. plan/tools/guest/jobs.sh  # ZEDBSD_JOBS, the parallel jobs (default 16)
build=${BUILD:-build/ws076-amd64}
count=2000
if [ "${1:-}" = "--count" ]; then
	count=$2
	shift 2
fi
config=plan/tools/libm/config-amd64-libm.mk
out=build/ws076-libm-guest
mkdir -p "$out"

# The C library of the image, which the runner links against.
make -j"$ZEDBSD_JOBS" BUILD_JOBS="$ZEDBSD_JOBS" CONFIGMK=$config BUILD="$build" "$build/dynamic/libc.so"

# The runner, compiled like a base program and linked with that libc.so.
sysroot=$root/build/amd64/sysroot
cc="$root/build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot=$sysroot"
$cc -nostdinc -I. -Iinclude -isystem "$sysroot/usr/include" -DHAL_ARCH_AMD64 \
	-DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC -m64 -march=x86-64 -mno-red-zone \
	-Os -ffreestanding -fPIC -fno-builtin -fno-stack-protector -Wall -Wextra \
	-Werror -c plan/tools/libm/libm-test.c -o "$out/libm-test.o"
$cc -m64 -nostdlib -pie -Wl,--no-relax -Wl,--hash-style=sysv,-z,now,-z,relro,-z,separate-code \
	-Wl,-z,stack-size=0x100000 -Wl,--dynamic-linker=/lib/ld.so \
	"$sysroot/usr/lib/crt1.o" "$out/libm-test.o" -L"$build/dynamic" \
	-l:libc.so -o "$out/libm-test"

# The reference cases, fewer than on the host so the image stays small.
reference="$out/reference-$count.bin"
if [ ! -f "$reference" ] || [ plan/tools/libm/gen-reference.py -nt "$reference" ]; then
	python3 plan/tools/libm/gen-reference.py "$reference" --count "$count"
fi

# The image with both files in /root.
# The firmware the configuration selects is fetched after its license is accepted once (ws233-p001); the question
# is answered the way CI answers it (2026-10-14, Q1 for the user).
yes | make CONFIGMK=$config BUILD="$build" firmware-licenses
make -j"$ZEDBSD_JOBS" BUILD_JOBS="$ZEDBSD_JOBS" CONFIGMK=$config BUILD="$build" \
	ROOTFS_EXTRA_FILES="--file /root/libm-test=$root/$out/libm-test --file /root/libm-ref.bin=$root/$reference --mode /root/libm-test=0755" \
	disk

# Boots it and runs the runner.
RUN=${RUN:-build/ws076-serial-run} TIMEOUT=${TIMEOUT:-600} \
	sh plan/tools/guest/amd64-serial.sh "$build/hdd-image.img" \
	"/root/libm-test /root/libm-ref.bin $*"

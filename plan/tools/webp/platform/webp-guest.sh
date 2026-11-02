#!/bin/sh
# libwebp-compat in a zedBSD guest: builds plan/tools/webp/webp-driver.c for zedBSD against BUILD's
# /lib/libwebp-compat.so, builds BUILD's image (config-amd64-webp.mk) unless IMAGE=existing is given, starts the guest
# (its own runtime directory, a free SSH port), sends the driver and every fixture in one ustar archive, runs the
# driver on each file there, fetches the reports back over SSH and compares each with the report the same driver
# built against libwebp 1.5.0 wrote on the host (FIXTURES/expect/NAME.ref).  The console and serial logs are not
# read.  make-fixtures.sh (and for the second set make-conformance.sh) must have run.
#   sh plan/tools/webp/platform/webp-guest.sh [FIXTURES...]
#     FIXTURES: fixture directories (default build/tmp/webp-fixtures); BUILD=build/webp-guest by default;
#     IMAGE=existing to reuse BUILD/hdd-image.img; KEEP=1 to leave the guest running.
# Last line: webp-guest: PASS (<n> files) or webp-guest: FAIL (...).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
root=$(pwd)
build=${BUILD:-build/webp-guest}
fixtures=${*:-build/tmp/webp-fixtures}
GUEST_RUNTIME=$root/build/webp-guest-run
export GUEST_RUNTIME
. plan/tools/fresh-out.sh
fresh_out build/tmp/webp-guest-out
out=$root/build/tmp/webp-guest-out
sysroot=/home/awe/zedBSD-claude1/build/amd64/sysroot
cc="$root/build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot=$sysroot"
guest() { timeout 900 /usr/bin/python3 plan/tools/guest/guest.py "$@" 2>&1 </dev/null; }

# The image, with the library under test in /lib.
if [ "${IMAGE:-}" != existing ]; then
	timeout 7200 plan/tools/guest/test-image.sh plan/tools/webp/platform/config-amd64-webp.mk "$build" \
		> "$out/image.log" 2>&1 || { tail -20 "$out/image.log"; echo "webp-guest: FAIL (image)"; exit 1; }
	sync
fi
[ -f "$build/hdd-image.img" ] && [ -f "$build/dynamic/libwebp-compat.so" ] ||
	{ echo "webp-guest: FAIL (no $build/hdd-image.img or libwebp-compat.so)"; exit 1; }

# The driver, linked with the library the image has.
$cc -nostdinc -Iinclude/libc -isystem "$sysroot/usr/include" -m64 -march=x86-64 -mno-red-zone -Os -ffreestanding \
	-fPIC -fno-builtin -fno-stack-protector -Wall -Wextra -Werror -c plan/tools/webp/webp-driver.c \
	-o "$out/webp-driver.o" || { echo "webp-guest: FAIL (driver compile)"; exit 1; }
$cc -m64 -nostdlib -pie -Wl,--no-relax -Wl,--hash-style=sysv,-z,now,-z,relro,-z,separate-code \
	-Wl,--dynamic-linker=/lib/ld.so "$sysroot/usr/lib/crt1.o" "$out/webp-driver.o" \
	-L"$build/dynamic" -l:libwebp-compat.so -l:libc.so -o "$out/webp-driver" || { echo "webp-guest: FAIL (driver link)"; exit 1; }

# The archive: the driver and the files of each set, named by set.
mkdir -p "$out/stage/files"
cp "$out/webp-driver" "$out/stage/webp-driver"
for set in $fixtures; do
	tag=$(basename "$set")
	for file in "$set"/files/*.webp; do
		cp "$file" "$out/stage/files/$tag--$(basename "$file")"
	done
done
(cd "$out/stage" && tar --format=ustar -cf "$out/webp-guest.tar" webp-driver files) || {
	echo "webp-guest: FAIL (archive)"; exit 1; }

# The guest, the archive into it, and the driver over every file there.
guest start "$build/hdd-image.img" > "$out/start.log" || { cat "$out/start.log"; echo "webp-guest: FAIL (start)"; exit 1; }
guest wait >> "$out/start.log"
ready=$(guest run 'echo ready' | tail -1)
if [ "$ready" != ready ]; then
	cat "$out/start.log"
	[ "${KEEP:-}" = 1 ] || guest stop > /dev/null
	echo "webp-guest: FAIL (no SSH to the guest)"
	exit 1
fi
guest run 'rm -rf /root/webp && mkdir -p /root/webp/out' > /dev/null
guest put "$out/webp-guest.tar" /root/webp/webp-guest.tar > /dev/null
guest run 'cd /root/webp && pax -r -f webp-guest.tar && chmod 755 webp-driver &&
	for f in files/*.webp; do n=${f#files/}; ./webp-driver "$f" > "out/${n%.webp}.txt" 2>&1; done;
	cd out && pax -w -f /root/webp/out.tar . && ls | wc -l' > "$out/run.log"
guest get /root/webp/out.tar "$out/out.tar" > /dev/null
[ "${KEEP:-}" = 1 ] || guest stop > /dev/null
sync

# The reports against libwebp's.
mkdir -p "$out/reports"
(cd "$out/reports" && tar -xf "$out/out.tar") || { echo "webp-guest: FAIL (no reports back)"; exit 1; }
total=0
failed=0
for set in $fixtures; do
	tag=$(basename "$set")
	for ref in "$set"/expect/*.ref; do
		name=$(basename "$ref" .ref)
		total=$((total + 1))
		report=$out/reports/$tag--$name.txt
		# The driver names the file it read; the guest's copy has the set's prefix.
		if ! sed "s/^file $tag--/file /" "$report" 2>/dev/null | cmp -s - "$ref"; then
			failed=$((failed + 1))
			echo "$tag/$name" >> "$out/failed.txt"
		fi
	done
done
if [ "$failed" = 0 ] && [ "$total" -gt 0 ]; then echo "webp-guest: PASS ($total files)"; exit 0; fi
head -10 "$out/failed.txt" 2>/dev/null
echo "webp-guest: FAIL ($failed of $total differ; $out/failed.txt)"
exit 1

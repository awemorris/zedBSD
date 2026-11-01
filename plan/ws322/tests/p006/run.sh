#!/bin/sh
# Host checks of the H.265 decode path (ws322-p006): ps-host-test.c over every HEVC_v1 conformance bitstream
# (plan/tools/h265/conformance/list.txt, read from the cache with unzip -p): each VPS, SPS and PPS through libmedia's
# h265-std, libvulkan's wire (encode.c), the PowerVR executor's readers and the engine's writers (executor.c) and
# libmedia's parse again, and each picture's record through the wire into the executor's record; then the engine's
# H.265 checks (engine-h265-host-test.c over a stand-in core).
#   plan/ws322/tests/p006/run.sh OUTDIR [NAME...]
# The cache is $H265SW_CONFORMANCE_DIR (default /home/awe/zedBSD-claude1/build/sources/h265-conformance, read only).
# Last line: p006: PASS (<n> streams) or p006: FAIL (<n> streams).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd -- "$(dirname -- "$0")/../../../.." && pwd)
here=$root/plan/ws322/tests/p006
out=$1
shift
mkdir -p "$out/inc"
ln -sfn "$root/include/libc/vulkan" "$out/inc/vulkan"
cache=${H265SW_CONFORMANCE_DIR:-/home/awe/zedBSD-claude1/build/sources/h265-conformance}
list=$root/plan/tools/h265/conformance/list.txt
san="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=all"
kernel="-std=gnu11 -O1 -g -D_POSIX_C_SOURCE=200809L -DKERN_USER_ABI_LP64 -include time.h -Wall -Wextra -Werror $san \
-ffunction-sections -fdata-sections -I$out/inc -I$root/include -I$root/src -I$root"
media="-std=gnu11 -O1 -g -D_DEFAULT_SOURCE -Wall -Wextra -Werror $san -ffunction-sections -fdata-sections -I$out/inc -I$root"

# The three sides and what they use.
objects=""
for file in h265-stream h265-dpb h265-std h265sw-nal h265sw-poc h265sw-ps h265sw-rps h265sw-scaling h265sw-slice h265sw-vui \
    vcodec-bitreader vcodec-nal bits; do
	timeout 300 cc $media -c -o "$out/$file.o" "$root/userland/desktop/libmedia/$file.c"
	objects="$objects $out/$file.o"
done
timeout 300 cc $media -c -o "$out/ps-host-test.o" "$here/ps-host-test.c"
timeout 300 cc $kernel -Wno-unused-function -c -o "$out/encode.o" "$here/encode.c"
timeout 300 cc $kernel -c -o "$out/lv-wire.o" "$root/userland/desktop/libvulkan/wire.c"
timeout 300 cc $kernel -c -o "$out/lv-codec.o" "$root/userland/desktop/libvulkan/codec.c"
timeout 300 cc $kernel -Wno-unused-function -c -o "$out/executor.o" "$here/executor.c"
timeout 300 cc $kernel -c -o "$out/i915-codec.o" "$root/src/drivers/gpu/i915/vk/vk-codec.c"
timeout 300 cc $kernel -c -o "$out/image-state.o" "$root/src/drivers/gpu/powervr/image-state.c"
timeout 300 cc $kernel -c -o "$out/vk-video-h265.o" "$root/src/drivers/gpu/powervr/vk/vk-video-h265.c"
timeout 300 cc $kernel -c -o "$out/vpu-h265-ps.o" "$root/src/drivers/vpu/vpu-h265-ps.c"
timeout 300 cc $kernel -c -o "$out/vpu-bits.o" "$root/src/drivers/vpu/vpu-bits.c"
timeout 300 cc $san -Wl,--gc-sections -o "$out/ps-host-test" "$out/ps-host-test.o" $objects "$out/encode.o" "$out/lv-wire.o" \
	"$out/lv-codec.o" "$out/executor.o" "$out/i915-codec.o" "$out/image-state.o" "$out/vk-video-h265.o" "$out/vpu-h265-ps.o" \
	"$out/vpu-bits.o"

# Each stream.
names="$*"
if [ -z "$names" ]; then
	names=$(grep -v '^#' "$list" | cut -f1)
fi
: > "$out/summary.txt"
streams=0
failed=0
for name in $names; do
	zip="$cache/$name.zip"
	member=$(unzip -Z1 "$zip" | grep -v '__MACOSX\|/\._\|^\._' | grep -i '\.\(bit\|bin\)$' | head -1)
	streams=$((streams + 1))
	if [ -z "$member" ]; then
		echo "$name FAIL (no bitstream)" >> "$out/summary.txt"
		failed=$((failed + 1))
		continue
	fi
	if unzip -p "$zip" "$member" | ASAN_OPTIONS=detect_leaks=1 timeout 600 "$out/ps-host-test" - > "$out/$name.txt" 2>&1; then
		echo "$name PASS $(tail -1 "$out/$name.txt")" >> "$out/summary.txt"
	else
		echo "$name FAIL $(tail -1 "$out/$name.txt")" >> "$out/summary.txt"
		failed=$((failed + 1))
	fi
done

# The engine's H.265 checks.
timeout 300 cc -std=c11 -O1 -g -Wall -Wextra -Werror $san -I"$root/include" -I"$root/src" \
	"$root/src/drivers/vpu/wave5-video.c" "$root/src/drivers/vpu/vpu-h264-ps.c" "$root/src/drivers/vpu/vpu-h265-ps.c" \
	"$root/src/drivers/vpu/vpu-bits.c" "$root/src/drivers/vpu/vpu-slots.c" "$here/engine-h265-host-test.c" \
	-o "$out/engine-h265-host-test"
if ! timeout 300 "$out/engine-h265-host-test" > "$out/engine.txt" 2>&1; then
	failed=$((failed + 1))
fi
tail -1 "$out/engine.txt"

# The verdict.
grep -c ' PASS ' "$out/summary.txt" | sed 's/^/streams passed: /'
grep ' FAIL ' "$out/summary.txt" | head -20 || true
if [ "$failed" = 0 ]; then
	echo "p006: PASS ($streams streams)"
	exit 0
fi
echo "p006: FAIL ($failed of $streams streams and the engine)"
exit 1

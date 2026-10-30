#!/bin/sh
# ws291-p021: the card image handed to the user, made from the Pi 5 measurement build (rpi5-image.sh): BUILD's
# hdd-image.img copied to OUT, its boot (FAT) partition given the firmware framebuffer of 1920x1080 (the screen the
# compositor scans out, in place of the CI image's 1280x720) and, with FALLBACK, a second kernel as vmunix-fallback
# (an older tree's build, booted by changing config.txt's kernel=vmunix to kernel=vmunix-fallback on any PC).
# cmdline.txt becomes CMDLINE (default: the console login and the V3D cost report every two seconds, v3d.perf=1; the
# GPU stays held until root's sysctl hw.gpu.start=1).
#   [FALLBACK=KERNEL] [CMDLINE='...'] sh plan/ws291/tests/rpi5-image-boot.sh BUILD OUT
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
build=${1:?usage: rpi5-image-boot.sh BUILD OUT}
out=${2:?the output image}
[ ! -e "$out" ] || { echo "rpi5-image-boot: $out exists; name a new file" >&2; exit 1; }
cp "$build/hdd-image.img" "$out"
# The first partition is the FAT one the firmware reads.
start=$(/usr/sbin/sfdisk -J "$out" | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["partitiontable"]["partitions"][0]["start"])')
fat="$out@@$((start * 512))"
mkdir -p build/tmp
tmp=$(mktemp -d build/tmp/rpi5-boot.XXXXXX)
mtype -i "$fat" ::/config.txt > "$tmp/config.txt"
grep -qx 'framebuffer_width=1280' "$tmp/config.txt" && grep -qx 'framebuffer_height=720' "$tmp/config.txt" ||
    { echo "rpi5-image-boot: config.txt has no 1280x720 framebuffer lines" >&2; exit 1; }
sed -e 's/^framebuffer_width=1280$/framebuffer_width=1920/' -e 's/^framebuffer_height=720$/framebuffer_height=1080/' \
    "$tmp/config.txt" > "$tmp/config.new"
mcopy -o -i "$fat" "$tmp/config.new" ::/config.txt
printf '%s\n' "${CMDLINE:-login=console v3d.perf=1}" > "$tmp/cmdline.txt"
mcopy -o -i "$fat" "$tmp/cmdline.txt" ::/cmdline.txt
if [ -n "${FALLBACK:-}" ]; then
	mcopy -i "$fat" "$FALLBACK" ::/vmunix-fallback
	echo "rpi5-image-boot: vmunix-fallback cksum $(cksum < "$FALLBACK" | cut -d' ' -f1)"
fi
echo "rpi5-image-boot: boot partition: $(mdir -b -i "$fat" :: | tr '\n' ' ')"
mtype -i "$fat" ::/config.txt | grep -E '^(kernel|framebuffer_(width|height))='
mtype -i "$fat" ::/cmdline.txt | sed 's/^/cmdline: /'
echo "rpi5-image-boot: $(readlink -f "$out") $(stat -c %s "$out") bytes sha256 $(sha256sum "$out" | cut -d' ' -f1)"

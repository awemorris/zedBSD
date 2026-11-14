#!/bin/sh
# ws299: copies an x86_64 test image and puts another kernel into the copy's FAT boot partition (::/vmunix), so that
# one serial image (plan/tools/posix/config-amd64-serial.mk) serves a kernel built with another configuration of the
# same tree (config-qemu-cache128.mk, config-qemu-cache32.mk).  The FAT partition is found by trying each partition
# of the copy's table for a ::/vmunix (sfdisk and mtools).  The last line is "image-kernel: OUT" or an error.
#   sh plan/ws299/tests/image-kernel.sh IMAGE VMUNIX OUT
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
PATH=$PATH:/usr/sbin:/sbin
IMAGE=${1:?image}
KERNEL=${2:?vmunix}
OUT=${3:?out}
[ -f "$KERNEL" ] || { echo "image-kernel: no kernel $KERNEL" >&2; exit 1; }
cp "$IMAGE" "$OUT"
for start in $(sfdisk -d "$OUT" | sed -n 's/.*start= *\([0-9]*\),.*/\1/p'); do
	if mdir -i "$OUT@@$((start * 512))" ::/vmunix > /dev/null 2>&1; then
		mcopy -o -i "$OUT@@$((start * 512))" "$KERNEL" ::/vmunix
		echo "image-kernel: $OUT"
		exit 0
	fi
done
echo "image-kernel: no FAT partition with /vmunix in $IMAGE" >&2
exit 1

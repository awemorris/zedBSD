#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail
if test "$#" -lt 2; then
	echo "usage: $0 OUTPUT_ISO VMUNIX [KERNEL-ARGUMENT ...]" >&2
	exit 2
fi
output="$1"
vmunix="$2"
shift 2
arguments="zedbsd.root=0x80,1"
if test "$#" -ne 0; then
	arguments="$*"
fi
stage="${output%.iso}.grub-root"
rm -rf "$stage"
mkdir -p "$stage/boot/grub"
cp "$vmunix" "$stage/boot/vmunix"
cat >"$stage/boot/grub/grub.cfg" <<EOF
set timeout=0
set default=0
menuentry "zedBSD PC/AT" {
    multiboot /boot/vmunix $arguments
    boot
}
EOF
grub-mkrescue -o "$output" "$stage" >/dev/null
rm -rf "$stage"
echo "GRUB Multiboot ISO: $output"

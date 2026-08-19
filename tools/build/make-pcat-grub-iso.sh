#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail
if test "$#" -ne 2; then
	echo "usage: $0 OUTPUT_ISO VMUNIX" >&2
	exit 2
fi
output="$1"
vmunix="$2"
stage="${output%.iso}.grub-root"
rm -rf "$stage"
mkdir -p "$stage/boot/grub"
cp "$vmunix" "$stage/boot/vmunix"
cat >"$stage/boot/grub/grub.cfg" <<'EOF'
set timeout=0
set default=0
menuentry "zedBSD PC/AT" {
    multiboot /boot/vmunix zedbsd.root=0x80,1
    boot
}
EOF
grub-mkrescue -o "$output" "$stage" >/dev/null
rm -rf "$stage"
echo "GRUB Multiboot ISO: $output"

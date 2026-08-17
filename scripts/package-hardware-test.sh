#!/usr/bin/env bash
# Package an immutable image plus its hardware regression manifest.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
machine_class="${1:-}"
output="${2:-$repo/build/hardware-test}"
case "$machine_class" in
pc98) build_arch=pc98; image="$repo/build/pc98/hdd-image.img" ;;
amd64-uefi) build_arch=unified; image="$repo/build/unified/hdd-image.img" ;;
*) echo 'usage: package-hardware-test.sh pc98|amd64-uefi [output-directory]' >&2; exit 2 ;;
esac

"$repo/build.sh" hdd-image "$build_arch"
mkdir -p "$output"
cp -f "$image" "$output/zedbsd-$machine_class.img"
python3 "$repo/scripts/hardware-test-manifest.py" create --root "$repo" \
	--image "$output/zedbsd-$machine_class.img" --machine-class "$machine_class" \
	--output "$output/manifest.json"
cp -f "$repo/docs/hardware-test-runbook.md" "$output/RUNBOOK.md"
echo "hardware test package: $output"

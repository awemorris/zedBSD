#!/usr/bin/env bash
# Minimal native-loader ELF64 kernel-entry smoke test.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
build="$repo/build/amd64-pcat"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-amd64-entry.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

python3 "$repo/scripts/make-bios-hdd-image.py" --force --machine pcat \
	--stage1 "$build/bootloader/stage1.bin" \
	--stage2 "$build/bootloader/stage2.bin" \
	--kernel "$build/vmunix" "$tmp/entry.img"
python3 "$repo/scripts/check-bios-hdd-image.py" --machine pcat \
	--kernel "$build/vmunix" "$tmp/entry.img"

log="$tmp/entry.log"
qemu="${QEMU_PCAT_X86_64:-qemu-system-x86_64}"
"$qemu" -M pc -cpu qemu64 -m 64 -accel tcg -nic none -display none \
	-serial none -monitor none -no-reboot -no-shutdown -snapshot \
	-debugcon "file:$log" -global isa-debugcon.iobase=0xe9 \
	-drive "if=ide,format=raw,file=$tmp/entry.img" &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT
found=0
for _ in $(seq 1 100); do
	if test -f "$log" && grep -Fq 'A64 ENTRY PASS' "$log"; then
		found=1
		break
	fi
	if ! kill -0 "$pid" 2>/dev/null; then break; fi
	sleep 0.1
done
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
test "$found" -eq 1 || { test -f "$log" && cat "$log"; exit 1; }
cat "$log"
echo "amd64 PC/AT entry QEMU test: PASS"

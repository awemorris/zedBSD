#!/bin/sh
# zedimage-host's native layout with the root's and swap's GUIDs given (--root-partuuid, --swap-partuuid) and
# the native image's check of them (platform/amd64/tools/check-amd64-native-image.py): the table has the GUIDs,
# the check finds them in zedbsd.cfg, and refuses a configuration or a table that names another; a GUID that is
# not one and a GUID for another layout are refused.  Needs mtools.
#   sh plan/tools/installer/host/native-partuuid-test.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
fresh_out build/installer-test/native-partuuid
out=$fresh_dir
cc -std=c11 -O2 -Wall -Wextra -Werror tools/build/zedimage-host.c -o "$out/zedimage-host"
head -c 4194304 /dev/urandom > "$out/root.ufs"
head -c 2097152 /dev/urandom > "$out/swap"
echo kernel > "$out/vmunix"
echo loader > "$out/BOOTX64.EFI"
root=0f1e2d3c-4b5a-4968-8776-a5b4c3d2e1f0
swap=11223344-5566-4778-8899-aabbccddeeff
printf '%s\n' kernel=vmunix "rootpart=PARTUUID=$root" "swap0=PARTUUID=$swap" > "$out/zedbsd.cfg"
status=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; status=1; }
image() {
	"$out/zedimage-host" disk --machine pcat --layout native --kernel "$out/vmunix" --bootx64 "$out/BOOTX64.EFI" \
		--zedbsd-config "$out/zedbsd.cfg" --ufs-root "$out/root.ufs" --swapfile "$out/swap" --fat-size-mib 64 "$@"
}
check() {
	python3 -I platform/amd64/tools/check-amd64-native-image.py --kernel "$out/vmunix" --bootx64 "$out/BOOTX64.EFI" \
		--zedbsd-config "$out/zedbsd.cfg" --ufs-root "$out/root.ufs" --swap "$out/swap" "$@"
}
image --root-partuuid "$root" --swap-partuuid "$swap" "$out/a.img" > "$out/a.txt" 2>&1 && ok "an image with the GUIDs" || { cat "$out/a.txt"; bad "an image with the GUIDs"; }
check --root-partuuid "$root" --swap-partuuid "$swap" "$out/a.img" > "$out/c.txt" 2>&1 && ok "the check finds them" || { cat "$out/c.txt"; bad "the check finds them"; }
# The table's entries, read apart from the check: entry 3's and 4's unique GUIDs.
python3 -I - "$out/a.img" "$root" "$swap" <<'EOF2' && ok "the table has them" || bad "the table has them"
import sys, uuid
data = open(sys.argv[1], "rb").read(1024 + 4 * 128)
assert str(uuid.UUID(bytes_le=data[1024 + 2 * 128 + 16:1024 + 2 * 128 + 32])) == sys.argv[2]
assert str(uuid.UUID(bytes_le=data[1024 + 3 * 128 + 16:1024 + 3 * 128 + 32])) == sys.argv[3]
EOF2
other=99999999-5566-4778-8899-aabbccddeeff
if check --root-partuuid "$other" --swap-partuuid "$swap" "$out/a.img" > "$out/d.txt" 2>&1; then bad "another root GUID (accepted)"; else ok "another root GUID is refused"; fi
printf '%s\n' kernel=vmunix "rootpart=PARTUUID=$other" "swap0=PARTUUID=$swap" > "$out/zedbsd.cfg"
if check --root-partuuid "$root" --swap-partuuid "$swap" "$out/a.img" > "$out/e.txt" 2>&1; then bad "a configuration naming another (accepted)"; else grep -q 'does not name the root' "$out/e.txt" && ok "a configuration naming another is refused" || bad "a configuration naming another"; fi
if image --root-partuuid not-a-guid "$out/b.img" > "$out/f.txt" 2>&1; then bad "a text that is not a GUID (accepted)"; else grep -q 'invalid --root-partuuid' "$out/f.txt" && ok "a text that is not a GUID is refused" || bad "a text that is not a GUID"; fi
if image --root-partuuid "$root" --swap-partuuid "$root" "$out/g.img" > "$out/g.txt" 2>&1; then bad "the same GUID twice (accepted)"; else grep -q 'repeats' "$out/g.txt" && ok "the same GUID twice is refused" || bad "the same GUID twice"; fi
if "$out/zedimage-host" disk --machine pcat --root-partuuid "$root" "$out/h.img" > "$out/h.txt" 2>&1; then bad "a GUID for another layout (accepted)"; else grep -q 'native layout' "$out/h.txt" && ok "a GUID for another layout is refused" || bad "a GUID for another layout"; fi
image "$out/i.img" > "$out/i.txt" 2>&1 && ok "an image without the GUIDs (random ones, as before)" || bad "an image without the GUIDs"
[ $status -eq 0 ] && echo "native-partuuid-test: PASS" || echo "native-partuuid-test: FAIL"
exit $status

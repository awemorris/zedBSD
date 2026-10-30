#!/bin/sh
# The installer's units, index and live root (tools/build/installer-units.py, installer-live.py,
# installer-root-size.py) on a made-up root: the tarballs' format, owners, modes and directories, the index's
# records (units, requirements, boot files, set-user-ID files, firmware licences, items and the other packages),
# the live root's files, payload and additions, and the refusals (a requirement cycle, an item naming no unit,
# a hard link, a long name, a live root that differs).
#   sh plan/tools/installer/host/installer-units-test.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
fresh_out build/installer-test/installer-units
out=$fresh_dir
t=$out/root
umask 022
mkdir -p "$t/bin" "$t/lib/firmware" "$t/etc/keiland" "$t/usr/bin" "$t/usr/lib" "$t/usr/share/licenses/i915" \
	"$t/tmp" "$t/home" "$t/var/empty" "$t/usr/share/curl/empty"
chmod 1777 "$t/tmp"
for f in bin/sh etc/passwd etc/keiland/autologin usr/bin/su usr/bin/files lib/firmware/i915.bin \
	usr/share/licenses/i915/LICENSE usr/bin/curl usr/lib/libcurl.so.4 usr/lib/libz.so.1; do
	echo "$f" > "$t/$f"
done
chmod 4755 "$t/usr/bin/su"
chmod 0755 "$t/bin/sh" "$t/usr/bin/curl"
ln -s libcurl.so.4 "$t/usr/lib/libcurl.so"
tab=$(printf '\t')
cat > "$out/manifest" <<EOF2
bin/sh${tab}base${tab}-${tab}f${tab}0755
etc/passwd${tab}base${tab}-${tab}f${tab}0644
etc/keiland/autologin${tab}desktop${tab}sessiond${tab}f${tab}0644
usr/bin/su${tab}base${tab}su${tab}f${tab}4755
usr/bin/files${tab}desktop${tab}files${tab}f${tab}0644
lib/firmware/i915.bin${tab}firmware${tab}i915fw${tab}f${tab}0644
usr/share/licenses/i915/LICENSE${tab}firmware${tab}i915fw${tab}f${tab}0644
usr/bin/curl${tab}network-curl${tab}curl${tab}f${tab}0755
usr/lib/libcurl.so.4${tab}network-curl${tab}curl${tab}f${tab}0644
usr/lib/libcurl.so${tab}network-curl${tab}curl${tab}l${tab}0777
usr/lib/libz.so.1${tab}libs-zlib${tab}zlib${tab}f${tab}0644
EOF2
cat > "$out/owners" <<EOF2
P${tab}su${tab}base${tab}base/su${tab}basic${tab}4755${tab}${tab}su${tab}1.0
P${tab}files${tab}desktop${tab}desktop/files${tab}basic${tab}${tab}network/curl${tab}Files${tab}1.0
P${tab}i915fw${tab}firmware${tab}firmware/i915${tab}data${tab}${tab}${tab}i915 firmware${tab}1.0
P${tab}zlib${tab}packages/libs${tab}libs/zlib${tab}package${tab}${tab}${tab}zlib${tab}1.3
P${tab}curl${tab}packages/network${tab}network/curl${tab}package${tab}${tab}libs/zlib base/su${tab}curl${tab}8.22.0
EOF2
cat > "$out/items" <<EOF2
base${tab}-${tab}required${tab}Base System${tab}base
desktop${tab}-${tab}on${tab}Keiland Desktop${tab}desktop
packages${tab}-${tab}on${tab}Packages${tab}-
curl${tab}packages${tab}off${tab}curl${tab}network-curl
other${tab}packages${tab}off${tab}Other packages${tab}-
EOF2
echo kernel > "$out/vmunix"
echo loader > "$out/BOOTX64.EFI"
status=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; status=1; }
units() {
	python3 -I tools/build/installer-units.py --manifest "$out/manifest" --owners "$out/owners" --tree "$t" \
		--items "$out/items" --version 9.9 --mtime 1700000000 \
		--boot BOOTX64.EFI=esp:/EFI/BOOT/BOOTX64.EFI="$out/BOOTX64.EFI" --boot vmunix=/boot/vmunix="$out/vmunix" \
		--output "$1" > "$1.txt" 2>&1
}
units "$out/payload" && ok "units of a root that fits" || { cat "$out/payload.txt"; bad "units of a root that fits"; }
p=$out/payload
has() { grep -q "$1" "$p/index" && ok "index: $2" || bad "index: $2"; }
has "^format${tab}1$" "format"
has "^unit${tab}base${tab}Base System${tab}base${tab}9.9${tab}" "the base's group and version"
has "^unit${tab}network-curl${tab}curl${tab}package${tab}8.22.0${tab}.*${tab}base libs-zlib$" "curl needs base and zlib"
has "^unit${tab}desktop${tab}Keiland Desktop${tab}desktop${tab}9.9${tab}.*${tab}network-curl$" "the desktop needs curl"
has "^unit${tab}libs-zlib${tab}.*${tab}-$" "zlib needs nothing"
has "^boot${tab}vmunix${tab}/boot/vmunix${tab}7${tab}" "the kernel"
has "^boot${tab}BOOTX64.EFI${tab}esp:/EFI/BOOT/BOOTX64.EFI${tab}" "the loader"
has "^setid${tab}base${tab}/usr/bin/su${tab}4755$" "su is set-user-ID"
has "^license${tab}firmware${tab}i915${tab}/usr/share/licenses/i915/LICENSE$" "the firmware's licence"
has "^item${tab}curl${tab}packages${tab}off${tab}curl${tab}network-curl$" "a fixed item"
has "^item${tab}other/libs-zlib${tab}other${tab}off${tab}zlib${tab}libs-zlib$" "zlib under other"
grep -q "^item${tab}other/network-curl" "$p/index" && bad "curl is not under other" || ok "curl is not under other"
# The tarballs, read by a reader of their own format.
python3 -I - "$p" "$t" <<'EOF2' && ok "tarballs" || bad "tarballs"
import hashlib, os, sys, tarfile
payload, tree = sys.argv[1], sys.argv[2]
index = [line.rstrip("\n").split("\t") for line in open(os.path.join(payload, "index"))]
for record in index:
	if record[0] != "unit":
		continue
	path = os.path.join(payload, "units", record[1] + ".tar.gz")
	data = open(path, "rb").read()
	assert hashlib.sha256(data).hexdigest() == record[8], record[1]
	assert len(data) == int(record[5]), record[1]
	with tarfile.open(path, "r:gz") as archive:
		members = archive.getmembers()
		assert len(members) == int(record[7]), (record[1], len(members), record[7])
		payload_bytes = 0
		for member in members:
			assert member.uid == 0 and member.gid == 0, member.name
			assert member.mtime == 1700000000, member.name
			assert not member.name.startswith(("/", "./")), member.name
			if member.isfile():
				payload_bytes += member.size
				assert archive.extractfile(member).read() == open(os.path.join(tree, member.name), "rb").read()
		assert payload_bytes == int(record[6]), record[1]
		names = {member.name: member for member in members}
		if record[1] == "base":
			assert names["usr/bin/su"].mode == 0o4755
			assert names["tmp"].isdir() and names["tmp"].mode == 0o1777
			assert "home" in names and "var/empty" in names
			assert "usr/share/curl/empty" in names, "an empty directory is the base's"
			assert "lib/firmware" not in names, "a directory of the firmware's alone is not the base's"
		if record[1] == "network-curl":
			assert names["usr/lib/libcurl.so"].issym() and names["usr/lib/libcurl.so"].linkname == "libcurl.so.4"
			assert names["usr/bin/curl"].mode == 0o755
			assert names["usr"].isdir() and "tmp" not in names
	assert open(path, "rb").read(10)[4:8] == b"\0\0\0\0", "gzip time 0"
	raw = __import__("gzip").decompress(data)
	assert raw[257:263] == b"ustar\0", record[1]
EOF2
# Another run of the same root makes the same index.
units "$out/payload2" && cmp -s "$p/index" "$out/payload2/index" && ok "the same root, the same index" || bad "the same root, the same index"
# The live root.
echo test > "$out/extra"
live() { python3 -I tools/build/installer-live.py --manifest "$out/manifest" --tree "$t" --unit base --unit desktop \
	--unit firmware --payload "$p" --file /etc/keiland/autologin="$out/extra" --file /usr/libexec/install-admin="$out/extra" \
	--mode /usr/libexec/install-admin=4550 --output "$1" > "$1.txt" 2>&1; }
live "$out/live" && ok "a live root" || { cat "$out/live.txt"; bad "a live root"; }
l=$out/live
[ -f "$l/usr/bin/files" ] && [ -f "$l/lib/firmware/i915.bin" ] && ok "the desktop and the firmware" || bad "the desktop and the firmware"
[ ! -e "$l/usr/bin/curl" ] && [ ! -e "$l/usr/lib/libz.so.1" ] && ok "no package" || bad "no package"
[ "$(stat -c %a "$l/usr/bin/su")" = 4755 ] && [ "$(stat -c %a "$l/tmp")" = 1777 ] && ok "modes" || bad "modes"
[ "$(cat "$l/etc/keiland/autologin")" = test ] && ok "a file replaced" || bad "a file replaced"
[ "$(stat -c %a "$l/usr/libexec/install-admin")" = 4550 ] && ok "the helper's mode" || bad "the helper's mode"
cmp -s "$l/opt/installer/index" "$p/index" && [ -f "$l/opt/installer/units/base.tar.gz" ] && ok "the payload" || bad "the payload"
[ -d "$l/usr/share/curl/empty" ] && ok "an empty directory" || bad "an empty directory"
size=$(python3 -I tools/build/installer-root-size.py --tree "$l" 2>/dev/null) && [ "$size" -ge 512 ] && ok "a root size ($size MiB)" || bad "a root size"
# Refusals.
refused() { if "$@"; then bad "$label (accepted)"; elif grep -q "$pattern" "$last"; then ok "$label"; else cat "$last"; bad "$label"; fi; }
n=0
refuse_units() { n=$((n + 1)); last=$out/r$n.txt; units "$out/r$n"; }
cp "$out/owners" "$out/owners.good"
printf 'P\tz2\tpackages/libs\tlibs/zlib\tpackage\t\tnetwork/curl\tzlib\t1\n' >> "$out/owners"
label="a requirement cycle is refused"; pattern="go round"; refused refuse_units
cp "$out/owners.good" "$out/owners"
printf 'python\tpackages\toff\tPython\tlang-python3\n' >> "$out/items"
label="an item naming no unit is refused"; pattern="names a unit the root has not: lang-python3"; refused refuse_units
grep -v '^python' "$out/items" > "$out/items.good"; cp "$out/items.good" "$out/items"
ln "$t/usr/lib/libz.so.1" "$t/usr/lib/libz.so"
printf 'usr/lib/libz.so\tlibs-zlib\tzlib\tf\t0644\n' >> "$out/manifest"
label="a hard link is refused"; pattern="a hard link"; refused refuse_units
grep -v '^usr/lib/libz.so	' "$out/manifest" > "$out/manifest.good"; cp "$out/manifest.good" "$out/manifest"
long=usr/share/$(printf 'x%.0s' $(seq 1 120))
echo long > "$t/$long"
printf '%s\tbase\t-\tf\t0644\n' "$long" >> "$out/manifest"
label="a name ustar cannot hold is refused"; pattern="name is too long\|too long"; refused refuse_units
cp "$out/manifest.good" "$out/manifest"
[ $status -eq 0 ] && echo "installer-units-test: PASS" || echo "installer-units-test: FAIL"
exit $status

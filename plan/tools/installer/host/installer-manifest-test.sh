#!/bin/sh
# The installer's manifest (tools/build/installer-manifest.py) on a made-up root: the base's own and listed files,
# a program's command, data, --file, --subtree and --link, a package's unit by its path, a desktop header from the
# sysroot, a path two programs claim (the required one wins), and the refusals (no unit, a test program's file, a
# set-user-ID file not asked for, a --file missing).
#   sh plan/tools/installer/host/installer-manifest-test.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
fresh_out build/installer-test/installer-manifest
out=$fresh_dir
t=$out/root
mkdir -p "$t/bin" "$t/lib" "$t/etc" "$t/usr/bin" "$t/usr/lib" "$t/usr/include/keiland" "$t/usr/include/sys" \
	"$t/usr/share/py/a" "$t/var" "$out/stage/a" "$out/inc/keiland"
for f in bin/sh lib/libc.so etc/passwd lib/arch.id etc/zedbsd-root usr/bin/curl usr/lib/libcurl.so.4 \
	usr/share/py/a/x.py usr/share/data.txt usr/include/keiland/keiland.h usr/include/sys/types.h usr/bin/su \
	usr/lib/libz.so.1 usr/bin/files; do
	echo "$f" > "$t/$f"
done
ln -s ../run "$t/var/run"
ln -s libcurl.so.4 "$t/usr/lib/libcurl.so"
chmod 4755 "$t/usr/bin/su"
echo x > "$out/stage/a/x.py"
echo h > "$out/inc/keiland/keiland.h"
tab=$(printf '\t')
cat > "$out/owners" <<EOF2
B
W${tab}--file
 W${tab}/etc/passwd=userland/base/etc/passwd
R
W${tab}--file
W${tab}/bin/sh=build/bin/sh
W${tab}--file
W${tab}/lib/libc.so=build/dynamic/libc.so
P${tab}su${tab}base${tab}base/su${tab}basic${tab}4755${tab}
C${tab}/usr/bin/su
W${tab}--file
W${tab}/etc/passwd=userland/base/etc/passwd
P${tab}files${tab}desktop${tab}desktop/files${tab}basic${tab}${tab}
C${tab}/usr/bin/files
D${tab}/usr/share/data.txt
P${tab}zlib${tab}packages/libs${tab}libs/zlib${tab}package${tab}${tab}
W${tab}--file
W${tab}/usr/lib/libz.so.1=x
P${tab}curl${tab}packages/network${tab}network/curl${tab}package${tab}${tab}libs/zlib
W${tab}--file
W${tab}/usr/bin/curl=x
W${tab}--file
W${tab}/usr/lib/libcurl.so.4=x
W${tab}--link
W${tab}/usr/lib/libcurl.so=libcurl.so.4
W${tab}--file
W${tab}/usr/lib/libz.so.1=x
P${tab}python${tab}packages/lang${tab}lang/python${tab}package${tab}${tab}
W${tab}--subtree
W${tab}/usr/share/py=$out/stage
EOF2
status=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; status=1; }
run() { python3 -I tools/build/installer-manifest.py --owners "$out/owners" --tree "$t" --desktop-include "$out/inc" --output "$out/manifest" > "$out/run.txt" 2>&1; }
expect() { grep -q "^$1	$2	" "$out/manifest" && ok "/$1 is $2" || bad "/$1 is $2: $(grep "^$1	" "$out/manifest" || echo none)"; }
run && ok "a root that fits" || { cat "$out/run.txt"; bad "a root that fits"; }
expect bin/sh base
expect etc/passwd base
expect lib/arch.id base
expect var/run base
expect usr/bin/su base
expect usr/bin/files desktop
expect usr/share/data.txt desktop
expect usr/bin/curl network-curl
expect usr/lib/libcurl.so network-curl
expect usr/lib/libz.so.1 libs-zlib
expect usr/share/py/a/x.py lang-python
expect usr/include/keiland/keiland.h desktop
expect usr/include/sys/types.h base
grep -q "^usr/bin/su	base	su	f	4755$" "$out/manifest" && ok "su keeps 4755" || bad "su keeps 4755"
# Refusals.
refused() { if run; then bad "$1 (accepted)"; elif grep -q "$2" "$out/run.txt"; then ok "$1"; else bad "$1"; fi; }
echo z > "$t/etc/stray"; refused "a file of no unit is refused" "no unit: /etc/stray"
mv "$t/etc/stray" "$out/stray.moved"
chmod 4755 "$t/usr/bin/files"; refused "set-user-ID not asked for is refused" "not asked for: /usr/bin/files"
chmod 0755 "$t/usr/bin/files"
printf 'P\tprobe\ttests\ttests/probe\tbasic\t\t\nC\t/usr/bin/files\n' >> "$out/owners"
refused "a test program's claim is refused" "claimed by\|not installed"
[ $status -eq 0 ] && echo "installer-manifest-test: PASS" || echo "installer-manifest-test: FAIL"
exit $status

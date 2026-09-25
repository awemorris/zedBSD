#!/bin/sh
# Extracts each archive with the guest's pax and compares with the host's listing.
cd "${2:-/root/paxtest}" || exit 1
f=$1
for f in $f; do
	rm -rf out; mkdir out; cd out
	pax -r -f ../$f.tar; echo "$f rc=$?"
	find . -type f | while read p; do cksum "$p"; done | sort > ../got-files.txt
	find . -type l | sort | while read l; do printf '%s -> %s\n' "$l" "$(readlink "$l")"; done > ../got-links.txt
	find . -type d | sort > ../got-dirs.txt
	cd ..
	if [ $f = ustar ]; then
		grep '^[0-9]* [0-9]* ./\(plain\|prefixonly\)/' expect-files.txt > expect-ustar.txt
		cmp got-files.txt expect-ustar.txt && echo "$f FILES-OK"
	else
		cmp got-files.txt expect-files.txt && echo "$f FILES-OK"
		cmp got-links.txt expect-links.txt && echo "$f LINKS-OK"
		cmp got-dirs.txt expect-dirs.txt && echo "$f DIRS-OK"
		ls -i out/long/short.txt out/long/hardlink.txt | awk '{print $1}' | uniq | wc -l | sed 's/^/inodes /'
		ls -l out/plain/fifo | cut -c1
	fi
	stat -c '%Y %n' out/plain/sub/x 2>/dev/null || ls -l out/plain/sub/x
	rm -rf out
	echo "$f listed $(pax -f $f.tar | wc -l)"
done

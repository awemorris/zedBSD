#!/bin/sh
# ws042-p011: compares two builds of ls (before and after a rewrite) over a
# matrix of options and operands on one tree: output and status must match.
#   sh plan/tools/utils/ls-compare.sh OLD_LS NEW_LS
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
old=$1 new=$2
root=$(mktemp -d)
cd "$root"
mkdir -p t/sub/deep t/empty t/.hid
echo hello > t/file; echo x > t/.dot; printf '%s' "$(seq 1 5000)" > t/big
dd if=/dev/zero of=t/huge bs=1024 count=3000 2>/dev/null
touch t/exec; chmod 755 t/exec; chmod 4755 t/exec
touch t/sgid; chmod 2644 t/sgid; chmod +t t/empty
ln -s file t/link; ln -s nowhere t/dangling; ln -s sub t/dirlink
mkfifo t/fifo
touch -t 200001020304 t/old; touch t/sub/a t/sub/deep/b
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do touch "t/name-with-length-$i"; done
fail=0
count=0
for opts in "" -a -l -la -1 -C -CF -Ci -F -i -li -d -ld -R -lR -r -t -tr -lt -h -lh -lah -L -lL -LF -aC -aCF -CR -ltr -lhd; do
	for ops in "t" "t/file t/sub" "t/sub t/file t/missing" "t/link t/dirlink" "t/dangling" "." "" "t/empty t/.hid"; do
		a=$(cd "$root" && $old $opts $ops 2>&1; echo "status=$?")
		b=$(cd "$root" && $new $opts $ops 2>&1; echo "status=$?")
		if [ "$a" != "$b" ]; then
			echo "DIFF: ls $opts $ops"; fail=1
			printf '%s\n' "$a" > /tmp/ls-a.txt; printf '%s\n' "$b" > /tmp/ls-b.txt; diff /tmp/ls-a.txt /tmp/ls-b.txt | head -5
		fi
		count=$((count+1))
	done
done
cd /; rm -rf "$root"
echo "compared=$count fail=$fail"

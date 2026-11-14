#!/bin/sh
# ws299: host test of plan/tools/hw5330/ufs-grow.py.  Makes small UFS volumes with the image build's own producer
# (mk/old-tools/build/zedimage-host.c), grows each in a file, and checks the grown volume with
# mk/old-tools/build/check-ufs-image.py (the
# validator fsck-like checks of plan/tools/ufs/check-volume.py: block ownership, bitmaps, summaries, files): the
# 1 GiB root's shape (65536 inodes, 256 groups of 4 MiB), a volume whose last group is partial, and a second run
# that has nothing to do.  The files written before the growth read back the same.  Prints PASS last.
#   sh plan/ws299/tests/host/ufs-grow-test.sh            (from the tree's top; work under build/tmp/ufs-grow)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
. plan/tools/fresh-out.sh
fresh_out build/tmp/ufs-grow
W=build/tmp/ufs-grow
# ZEDIMAGE_HOST: another copy of the producer (default one built here from the image build's source, as
# mk/platform-x86_64/image.mk builds it).
if [ -z "${ZEDIMAGE_HOST:-}" ]; then
	ZEDIMAGE_HOST=$W/zedimage-host
	cc -std=c11 -O2 -Wall -Wextra -Werror mk/old-tools/build/zedimage-host.c -o "$ZEDIMAGE_HOST"
fi
mkdir -p "$W/tree/etc" "$W/tree/bin"
head -c 300000 /dev/urandom > "$W/tree/bin/blob"
echo hello > "$W/tree/etc/motd"

check() {
	python3 -I - "$1" "$W/tree" << 'EOF'
import importlib.util, pathlib, sys
spec = importlib.util.spec_from_file_location('c', 'mk/old-tools/build/check-ufs-image.py')
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
fs = c.UFS(pathlib.Path(sys.argv[1]).read_bytes(), runtime=True)
fs.validate()
names = {name: number for name, number, _ in fs.entries(c.ROOT)}
bins = {name: number for name, number, _ in fs.entries(names['bin'])}
want = (pathlib.Path(sys.argv[2]) / 'bin/blob').read_bytes()
assert fs.read_file(bins['blob']) == want, 'blob differs'
print('check: %d groups, %d fragments' % (fs.ncg, fs.fragments))
EOF
}

# The 1 GiB root's shape, scaled: 65536 inodes force 256 groups, here of 256 KiB, grown sixfold.
"$ZEDIMAGE_HOST" ufs 64M "$W/tree" "$W/a.ufs" --inodes=65536
python3 -I plan/tools/hw5330/ufs-grow.py --check --size $((384 << 20)) "$W/a.ufs"
python3 -I plan/tools/hw5330/ufs-grow.py --size $((384 << 20)) "$W/a.ufs"
check "$W/a.ufs"
python3 -I plan/tools/hw5330/ufs-grow.py "$W/a.ufs" | grep -q 'already fills'

# A size that leaves the last group partial, and one that leaves too little for a last group.
"$ZEDIMAGE_HOST" ufs 32M "$W/tree" "$W/b.ufs"
python3 -I plan/tools/hw5330/ufs-grow.py --size $((100 << 20 | 12345 << 10)) "$W/b.ufs"
check "$W/b.ufs"
"$ZEDIMAGE_HOST" ufs 32M "$W/tree" "$W/c.ufs" --inodes=8192
python3 -I plan/tools/hw5330/ufs-grow.py --size $(((96 << 20) + (100 << 10))) "$W/c.ufs"
check "$W/c.ufs"
echo "ufs-grow-test: PASS"

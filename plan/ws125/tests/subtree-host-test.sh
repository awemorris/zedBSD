#!/bin/sh
# ws125-p002: a package's whole directory (--subtree DEST=DIRECTORY) reaches the root.
#
# A host test, no guest.  It makes a stage of more than 3000 files (nested
# directories, 0755 and 0644 files, an empty file, a link to a file and a link
# to a directory) and puts it at /usr/lib/aat-tree:
#   1. through the root Makefile's own ZEDBSD_ROOTFS_TREE_RULE (its define is
#      taken from the Makefile and evaluated by a small makefile), the rule
#      every disk image's root is staged by;
#   2. through tools/build/make-arch-overlay-ufs.noct (the test UFS images),
#      checked by tools/build/check-arch-overlay-ufs.py --subtree;
#   3. the FAT tool's expansion (tools/build/subtree_files.py) refuses the
#      links a FAT root cannot hold.
# Each part compares the root with the stage: the paths, the kinds, the sizes,
# the modes and the links' targets.  The last line is subtree-host-test: PASS
# or FAIL.  Output: build/ws125-subtree (a fresh directory each run).
#
#   plan/ws125/tests/subtree-host-test.sh [FILES]
#
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

set -u
cd "$(dirname "$0")/../../.." || exit 2
. plan/tools/fresh-out.sh
files=${1:-3200}
status=0
noct=build/NoctLang/build-static/noct
shell=build/amd64/bin/sh

# Reports one check's outcome.
result() {
	if [ "$2" -eq 0 ]; then
		echo "$1: ok"
	else
		echo "$1: FAIL"
		status=1
	fi
}

for tool in "$noct" build/zedimage-host "$shell"; do
	if [ ! -x "$tool" ]; then
		echo "missing $tool (build it first)"
		echo "subtree-host-test: FAIL"
		exit 1
	fi
done

fresh_out build/ws125-subtree
out=$(cd build/ws125-subtree && pwd -P)

# The stage: FILES files in 20 directories of 10, every seventh executable, one empty, and two links.
python3 -I - "$out/stage" "$files" <<'EOF'
import os, sys
stage, count = sys.argv[1], int(sys.argv[2])
for index in range(count):
    directory = os.path.join(stage, f"d{index % 20}", f"e{index % 10}")
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, f"file-{index}.py")
    with open(path, "w", encoding="ascii") as stream:
        stream.write("" if index == 5 else f"# file {index}\n" * (1 + index % 13))
    os.chmod(path, 0o755 if index % 7 == 0 else 0o644)
os.symlink("d0/e0/file-0.py", os.path.join(stage, "link-to-file"))
os.symlink("d1", os.path.join(stage, "link-to-directory"))
EOF
count=$(find "$out/stage" | wc -l)
echo "stage: $count entries"

# The listing of a tree: path, kind, size of a file, mode and a link's target.
listing() {
	(cd "$1" && find . -mindepth 1 \( -type l -printf '%p l %l\n' \) -o \( -type f -printf '%p f %s %m\n' \) -o \( -type d -printf '%p d\n' \)) | LC_ALL=C sort
}
listing "$out/stage" > "$out/stage.list"

# 1. The root Makefile's tree rule, evaluated alone.
sed -n '/^define ZEDBSD_ROOTFS_TREE_RULE$/,/^endef$/p' Makefile > "$out/tree-rule.mk"
printf 'config\n' > "$out/config-stamp"
cat > "$out/test.mk" <<EOF
BUILD := $out/build
ZEDBSD_ROOTFS_CONFIG_STAMP := $out/config-stamp
ZEDBSD_ROOTFS_DIRECTORIES := bin sbin lib etc var root home dev boot run usr/bin usr/sbin usr/libexec var/empty etc/ssh
ZEDBSD_ROOTFS_STICKY_DIRECTORIES := tmp shm
ZEDBSD_PACKAGE_INPUTS :=
ZEDBSD_ROOTFS_DEVELOPMENT_INPUTS :=
ZEDBSD_PACKAGE_LINKS :=
ZEDBSD_ROOTFS_INSTALL_DEVELOPMENT :=
ZEDBSD_PACKAGE_FILES := --subtree /usr/lib/aat-tree=$out/stage --mode /usr/lib/aat-tree/d0/e0/file-20.py=0600
include $out/tree-rule.mk
\$(eval \$(call ZEDBSD_ROOTFS_TREE_RULE,amd64,,--file /bin/sh=$PWD/$shell))
EOF
make -s -f "$out/test.mk" "$out/build/rootfs/.stamp" > "$out/make.log" 2>&1
result "rootfs tree rule ran" $?
listing "$out/build/rootfs/usr/lib/aat-tree" > "$out/rootfs.list"
# The --mode applied after the tree: 0600 where the stage has 0644; directories are 0755 in a root.
sed -e 's#^\(\./d0/e0/file-20\.py f [0-9]*\) 644$#\1 600#' "$out/stage.list" > "$out/expected.list"
cmp -s "$out/expected.list" "$out/rootfs.list"
result "rootfs tree matches the stage ($count entries)" $?
[ -x "$out/build/rootfs/bin/sh" ]
result "rootfs /bin/sh still installed" $?

# 2. The UFS image tool and its checker.
"$noct" --path=tools/build tools/build/make-arch-overlay-ufs.noct --backend "$PWD/build/zedimage-host" --force \
	--profile amd64 --output "$out/test.ufs" --file "/bin/sh=$shell" \
	--subtree "/usr/lib/aat-tree=$out/stage" --mode /usr/lib/aat-tree/d0/e0/file-20.py=0600 > "$out/ufs.log" 2>&1
result "UFS image made" $?
PYTHONPATH=tools/build python3 tools/build/check-arch-overlay-ufs.py --profile amd64 --image "$out/test.ufs" \
	--file "/bin/sh=$shell" --subtree "/usr/lib/aat-tree=$out/stage" --mode /usr/lib/aat-tree/d0/e0/file-20.py=0600 > "$out/ufs-check.log" 2>&1
result "UFS image checked (every file's bytes, the executables, the links)" $?

# 3. A FAT root refuses the links.
PYTHONPATH=tools/build python3 -c 'import sys; from subtree_files import subtree_file_specifications; subtree_file_specifications(sys.argv[1:])' \
	"/usr/lib/aat-tree=$out/stage" > "$out/fat.log" 2>&1
grep -q 'a FAT root holds no symbolic link' "$out/fat.log"
result "FAT expansion refuses links" $?

if [ "$status" -eq 0 ]; then
	echo "subtree-host-test: PASS"
else
	echo "subtree-host-test: FAIL (logs in $out)"
fi
exit "$status"
